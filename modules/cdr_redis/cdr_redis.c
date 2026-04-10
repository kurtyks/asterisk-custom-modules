/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2024
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \file
 * \brief Redis CDR backend
 *
 * This module stores CDR records as Redis Hashes. Each record is saved under
 * the key "<prefix><uniqueid>" and, optionally, serialised as JSON and
 * published to a Redis Pub/Sub channel.
 *
 * Requires the hiredis library (https://github.com/redis/hiredis).
 *
 * \ingroup cdr_drivers
 */

/*! \li \ref cdr_redis.c uses the configuration file \ref cdr_redis.conf
 * \addtogroup configuration_file Configuration Files
 */

/*!
 * \page cdr_redis.conf cdr_redis.conf
 * \verbinclude cdr_redis.conf.sample
 */

/*** MODULEINFO
	<depend>hiredis</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <hiredis/hiredis.h>

#include "asterisk/channel.h"
#include "asterisk/cdr.h"
#include "asterisk/config.h"
#include "asterisk/json.h"
#include "asterisk/module.h"
#include "asterisk/utils.h"

#define CONF_FILE        "cdr_redis.conf"
#define DATE_FORMAT      "%Y-%m-%d %T"

#define DEFAULT_HOST     "127.0.0.1"
#define DEFAULT_PORT     6379
#define DEFAULT_DATABASE 0
#define DEFAULT_PREFIX   "asterisk:cdr:"
#define DEFAULT_TTL      0   /* seconds; 0 = no expiry */

static const char name[] = "cdr_redis";

/*!
 * \brief Operation mode for the Redis CDR backend.
 *
 * REDIS_MODE_HASH   - Store CDRs as Redis Hashes only (HSET).
 * REDIS_MODE_PUBSUB - Publish CDRs as JSON to a Pub/Sub channel only (PUBLISH).
 * REDIS_MODE_BOTH   - Store as Hash and publish to Pub/Sub channel.
 */
enum redis_mode {
	REDIS_MODE_HASH   = 0,
	REDIS_MODE_PUBSUB = 1,
	REDIS_MODE_BOTH   = 2,
};

/* --------------------------------------------------------------------------
 * Configuration (protected by config_lock)
 * -------------------------------------------------------------------------- */

AST_RWLOCK_DEFINE_STATIC(config_lock);

static char *redis_host   = NULL;
static int   redis_port   = DEFAULT_PORT;
static char *redis_pass   = NULL;
static int   redis_db     = DEFAULT_DATABASE;
static char *key_prefix   = NULL;
static int   key_ttl      = DEFAULT_TTL;
static char *pub_channel  = NULL;
static enum redis_mode mode = REDIS_MODE_HASH;
static int   enablecdr    = 0;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/*! \brief Open a hiredis connection, authenticate and select the database.
 *
 * Returns a ready-to-use context on success, or NULL on failure.
 * The caller is responsible for calling redisFree() when done.
 */
static redisContext *redis_connect(void)
{
	redisContext *c;
	redisReply *reply;

	c = redisConnect(redis_host, redis_port);
	if (!c || c->err) {
		ast_log(LOG_ERROR, "cdr_redis: connection to %s:%d failed: %s\n",
		        redis_host, redis_port, c ? c->errstr : "out of memory");
		if (c) {
			redisFree(c);
		}
		return NULL;
	}

	if (!ast_strlen_zero(redis_pass)) {
		reply = redisCommand(c, "AUTH %s", redis_pass);
		if (!reply || reply->type == REDIS_REPLY_ERROR) {
			ast_log(LOG_ERROR, "cdr_redis: AUTH failed: %s\n",
			        reply ? reply->str : "no reply");
			freeReplyObject(reply);
			redisFree(c);
			return NULL;
		}
		freeReplyObject(reply);
	}

	if (redis_db != 0) {
		reply = redisCommand(c, "SELECT %d", redis_db);
		if (!reply || reply->type == REDIS_REPLY_ERROR) {
			ast_log(LOG_ERROR, "cdr_redis: SELECT %d failed: %s\n",
			        redis_db, reply ? reply->str : "no reply");
			freeReplyObject(reply);
			redisFree(c);
			return NULL;
		}
		freeReplyObject(reply);
	}

	return c;
}

/*! \brief Format a struct timeval as a string using DATE_FORMAT.
 *
 * An empty string is written when \p when is zero (unanswered call).
 */
static void format_time(struct timeval when, char *buf, size_t bufsize)
{
	struct ast_tm tm;

	if (ast_tvzero(when)) {
		buf[0] = '\0';
		return;
	}
	ast_localtime(&when, &tm, NULL);
	ast_strftime(buf, bufsize, DATE_FORMAT, &tm);
}

/* --------------------------------------------------------------------------
 * CDR callback
 * -------------------------------------------------------------------------- */

static int redis_log(struct ast_cdr *cdr)
{
	char strStartTime[80]  = "";
	char strAnswerTime[80] = "";
	char strEndTime[80]    = "";
	char strDuration[16];
	char strBillsec[16];
	char strSequence[16];
	char key[256];
	redisContext *c;
	redisReply   *reply;
	struct ast_json *json_cdr = NULL;
	char *json_str = NULL;
	int   ret = 0;

	if (!enablecdr) {
		return 0;
	}

	format_time(cdr->start,  strStartTime,  sizeof(strStartTime));
	format_time(cdr->answer, strAnswerTime, sizeof(strAnswerTime));
	format_time(cdr->end,    strEndTime,    sizeof(strEndTime));

	snprintf(strDuration, sizeof(strDuration), "%ld", cdr->duration);
	snprintf(strBillsec,  sizeof(strBillsec),  "%ld", cdr->billsec);
	snprintf(strSequence, sizeof(strSequence), "%d",  cdr->sequence);

	ast_rwlock_rdlock(&config_lock);

	snprintf(key, sizeof(key), "%s%s", key_prefix, cdr->uniqueid);

	c = redis_connect();
	if (!c) {
		ast_rwlock_unlock(&config_lock);
		return -1;
	}

	/* HSET — only in hash and both modes */
	if (mode == REDIS_MODE_HASH || mode == REDIS_MODE_BOTH) {
		reply = redisCommand(c,
			"HSET %s"
			" accountcode %s"
			" src %s"
			" dst %s"
			" dcontext %s"
			" clid %s"
			" channel %s"
			" dstchannel %s"
			" lastapp %s"
			" lastdata %s"
			" start %s"
			" answer %s"
			" end %s"
			" duration %s"
			" billsec %s"
			" disposition %s"
			" amaflags %s"
			" uniqueid %s"
			" linkedid %s"
			" userfield %s"
			" peeraccount %s"
			" sequence %s",
			key,
			S_OR(cdr->accountcode, ""),
			S_OR(cdr->src,         ""),
			S_OR(cdr->dst,         ""),
			S_OR(cdr->dcontext,    ""),
			S_OR(cdr->clid,        ""),
			S_OR(cdr->channel,     ""),
			S_OR(cdr->dstchannel,  ""),
			S_OR(cdr->lastapp,     ""),
			S_OR(cdr->lastdata,    ""),
			strStartTime,
			strAnswerTime,
			strEndTime,
			strDuration,
			strBillsec,
			S_OR(ast_cdr_disp2str(cdr->disposition),           ""),
			S_OR(ast_channel_amaflags2string(cdr->amaflags),   ""),
			S_OR(cdr->uniqueid,    ""),
			S_OR(cdr->linkedid,    ""),
			S_OR(cdr->userfield,   ""),
			S_OR(cdr->peeraccount, ""),
			strSequence);

		if (!reply || reply->type == REDIS_REPLY_ERROR) {
			ast_log(LOG_ERROR, "cdr_redis: HSET failed for key '%s': %s\n",
			        key, reply ? reply->str : "no reply");
			ret = -1;
		} else {
			ast_log(LOG_DEBUG, "cdr_redis: stored CDR as hash '%s'\n", key);
		}
		freeReplyObject(reply);

		if (ret == 0 && key_ttl > 0) {
			reply = redisCommand(c, "EXPIRE %s %d", key, key_ttl);
			if (!reply || reply->type == REDIS_REPLY_ERROR) {
				ast_log(LOG_WARNING, "cdr_redis: EXPIRE failed for key '%s'\n", key);
			}
			freeReplyObject(reply);
		}
	}

	/* PUBLISH — only in pubsub and both modes */
	if (ret == 0 && (mode == REDIS_MODE_PUBSUB || mode == REDIS_MODE_BOTH) && !ast_strlen_zero(pub_channel)) {
		json_cdr = ast_json_pack(
			"{s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s,"
			" s:s, s:s, s:s, s:i, s:i, s:s, s:s, s:s, s:s, s:s, s:s, s:i}",
			"accountcode",  S_OR(cdr->accountcode, ""),
			"src",          S_OR(cdr->src,          ""),
			"dst",          S_OR(cdr->dst,          ""),
			"dcontext",     S_OR(cdr->dcontext,     ""),
			"clid",         S_OR(cdr->clid,         ""),
			"channel",      S_OR(cdr->channel,      ""),
			"dstchannel",   S_OR(cdr->dstchannel,   ""),
			"lastapp",      S_OR(cdr->lastapp,      ""),
			"lastdata",     S_OR(cdr->lastdata,     ""),
			"start",        strStartTime,
			"answer",       strAnswerTime,
			"end",          strEndTime,
			"duration",     (int)cdr->duration,
			"billsec",      (int)cdr->billsec,
			"disposition",  S_OR(ast_cdr_disp2str(cdr->disposition),         ""),
			"amaflags",     S_OR(ast_channel_amaflags2string(cdr->amaflags), ""),
			"uniqueid",     S_OR(cdr->uniqueid,     ""),
			"linkedid",     S_OR(cdr->linkedid,     ""),
			"userfield",    S_OR(cdr->userfield,    ""),
			"peeraccount",  S_OR(cdr->peeraccount,  ""),
			"sequence",     cdr->sequence);

		if (json_cdr) {
			json_str = ast_json_dump_string(json_cdr);
			if (json_str) {
				reply = redisCommand(c, "PUBLISH %s %s", pub_channel, json_str);
				if (!reply || reply->type == REDIS_REPLY_ERROR) {
					ast_log(LOG_WARNING,
					        "cdr_redis: PUBLISH to channel '%s' failed: %s\n",
					        pub_channel, reply ? reply->str : "no reply");
				}
				freeReplyObject(reply);
				ast_json_free(json_str);
			}
			ast_json_unref(json_cdr);
		}
	}

	redisFree(c);
	ast_rwlock_unlock(&config_lock);

	return ret;
}

/* --------------------------------------------------------------------------
 * Configuration loading
 * -------------------------------------------------------------------------- */

static int load_config(int reload)
{
	struct ast_config   *cfg;
	struct ast_variable *v;
	struct ast_flags     config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	int newenablecdr = 0;

	cfg = ast_config_load(CONF_FILE, config_flags);

	if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	}

	if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "cdr_redis: config file '%s' is invalid\n", CONF_FILE);
		return -1;
	}

	if (!cfg) {
		ast_log(LOG_WARNING, "cdr_redis: config file '%s' not found; module inactive\n",
		        CONF_FILE);
		if (enablecdr) {
			ast_cdr_backend_suspend(name);
		}
		enablecdr = 0;
		return -1;
	}

	if (reload) {
		ast_rwlock_wrlock(&config_lock);
		ast_free(redis_host);
		ast_free(redis_pass);
		ast_free(key_prefix);
		ast_free(pub_channel);
		redis_host  = NULL;
		redis_pass  = NULL;
		key_prefix  = NULL;
		pub_channel = NULL;
	}

	/* Defaults */
	redis_host  = ast_strdup(DEFAULT_HOST);
	redis_port  = DEFAULT_PORT;
	redis_pass  = NULL;
	redis_db    = DEFAULT_DATABASE;
	key_prefix  = ast_strdup(DEFAULT_PREFIX);
	key_ttl     = DEFAULT_TTL;
	pub_channel = NULL;
	mode        = REDIS_MODE_HASH;

	for (v = ast_variable_browse(cfg, "general"); v; v = v->next) {
		if (!strcasecmp(v->name, "enabled")) {
			newenablecdr = ast_true(v->value);
		} else if (!strcasecmp(v->name, "host")) {
			ast_free(redis_host);
			redis_host = ast_strdup(v->value);
		} else if (!strcasecmp(v->name, "port")) {
			if (sscanf(v->value, "%d", &redis_port) != 1 || redis_port <= 0) {
				ast_log(LOG_WARNING, "cdr_redis: invalid port '%s', using default %d\n",
				        v->value, DEFAULT_PORT);
				redis_port = DEFAULT_PORT;
			}
		} else if (!strcasecmp(v->name, "password")) {
			ast_free(redis_pass);
			redis_pass = ast_strdup(v->value);
		} else if (!strcasecmp(v->name, "database")) {
			if (sscanf(v->value, "%d", &redis_db) != 1 || redis_db < 0) {
				ast_log(LOG_WARNING, "cdr_redis: invalid database '%s', using default %d\n",
				        v->value, DEFAULT_DATABASE);
				redis_db = DEFAULT_DATABASE;
			}
		} else if (!strcasecmp(v->name, "key_prefix")) {
			ast_free(key_prefix);
			key_prefix = ast_strdup(v->value);
		} else if (!strcasecmp(v->name, "ttl")) {
			if (sscanf(v->value, "%d", &key_ttl) != 1 || key_ttl < 0) {
				ast_log(LOG_WARNING, "cdr_redis: invalid ttl '%s', using default %d\n",
				        v->value, DEFAULT_TTL);
				key_ttl = DEFAULT_TTL;
			}
		} else if (!strcasecmp(v->name, "mode")) {
			if (!strcasecmp(v->value, "hash")) {
				mode = REDIS_MODE_HASH;
			} else if (!strcasecmp(v->value, "pubsub")) {
				mode = REDIS_MODE_PUBSUB;
			} else if (!strcasecmp(v->value, "both")) {
				mode = REDIS_MODE_BOTH;
			} else {
				ast_log(LOG_WARNING, "cdr_redis: unknown mode '%s', using 'hash'\n", v->value);
				mode = REDIS_MODE_HASH;
			}
		} else if (!strcasecmp(v->name, "channel")) {
			ast_free(pub_channel);
			pub_channel = ast_strdup(v->value);
		}
	}

	if (reload) {
		ast_rwlock_unlock(&config_lock);
	}

	ast_config_destroy(cfg);

	if (!newenablecdr) {
		ast_cdr_backend_suspend(name);
	} else {
		ast_cdr_backend_unsuspend(name);
		ast_log(LOG_NOTICE,
		        "cdr_redis: configured – host=%s port=%d db=%d mode=%s%s%s\n",
		        redis_host, redis_port, redis_db,
		        mode == REDIS_MODE_HASH   ? "hash"   :
		        mode == REDIS_MODE_PUBSUB ? "pubsub" : "both",
		        pub_channel ? " channel=" : "",
		        pub_channel ? pub_channel : "");
	}
	enablecdr = newenablecdr;

	return 0;
}

/* --------------------------------------------------------------------------
 * Module lifecycle
 * -------------------------------------------------------------------------- */

static int unload_module(void)
{
	if (ast_cdr_unregister(name)) {
		return -1;
	}

	ast_rwlock_wrlock(&config_lock);
	ast_free(redis_host);
	ast_free(redis_pass);
	ast_free(key_prefix);
	ast_free(pub_channel);
	redis_host  = NULL;
	redis_pass  = NULL;
	key_prefix  = NULL;
	pub_channel = NULL;
	ast_rwlock_unlock(&config_lock);

	return 0;
}

static int load_module(void)
{
	if (ast_cdr_register(name, "Asterisk Redis CDR Backend", redis_log)) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (load_config(0)) {
		ast_cdr_unregister(name);
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	return load_config(1);
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Asterisk Redis CDR Backend",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
	.load_pri = AST_MODPRI_CDR_DRIVER,
	.requires = "cdr",
);
