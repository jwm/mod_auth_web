/*
 * mod_auth_web - URL-based authentication for ProFTPD
 * Copyright (c) 2006-7, 2011, 2014, John Morrissey <jwm@horde.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, 5th Floor, Boston, MA 02110-1301, USA.
 */

/* $Libraries: -lcurl$ */
#include <curl/curl.h>

#include "conf.h"
#include "privs.h"

#define MOD_AUTH_WEB_VERSION  "mod_auth_web/1.1.2"

/* Config values */
static char *local_user;
static char *url, *user_param_name, *pass_param_name;
static char *failed_string;
static array_header *required_headers, *received_headers;

static pr_regex_t *user_creg;
static char *response_data;

module auth_web_module;

static char *urlencode(pool *p, const char *str);


MODRET
handle_auth_web_getpwnam(cmd_rec *cmd)
{
	struct passwd *pw;

	if (!url || !user_param_name || !pass_param_name || !local_user ||
	    !(failed_string || required_headers)) {
		return PR_DECLINED(cmd);
	}
	if (user_creg) {
		if (pr_regexp_exec(user_creg, cmd->argv[0], 0, NULL, 0, 0, 0) != 0) {
			pr_log_pri(PR_LOG_DEBUG, MOD_AUTH_WEB_VERSION ": user doesn't match regex");
			return PR_DECLINED(cmd);
		}
	}

	pw = pcalloc(session.pool, sizeof(struct passwd));
	if (!pw) {
		return PR_DECLINED(cmd);
	}

	memcpy(pw, getpwnam(local_user), sizeof(struct passwd));
	pw->pw_name = pstrdup(session.pool, cmd->argv[0]);
	if (!pw->pw_name) {
		return PR_DECLINED(cmd);
	}
	return mod_create_data(cmd, pw);
}

static size_t
get_response_headers(const void *buffer, const size_t size,
                     const size_t nmemb, const void *userp)
{
	char *str;
	unsigned int copy_len;

	/* libcurl doesn't guarantee NULL termination, so make sure ourselves.
	 * Don't copy the trailing CR/LF, if present.
	 */
	str = (char *) buffer;
	copy_len = size * nmemb;
	if (str[copy_len - 1] == '\r' || str[copy_len - 1] == '\n') {
		--copy_len;
	}
	if (str[copy_len - 1] == '\r' || str[copy_len - 1] == '\n') {
		--copy_len;
	}

	str = pstrndup(session.pool, str, copy_len);
	if (!str) {
		return 0;
	}
	pr_log_pri(PR_LOG_DEBUG, MOD_AUTH_WEB_VERSION ": received response header: %s", (char *) str);

	if (received_headers == NULL) {
		/* 16 is an arbitrary, but probably reasonable, number. */
		received_headers = make_array(session.pool, 16, sizeof(char *));
		if (received_headers == NULL) {
			return 0;
		}
	}
	*((char **) push_array(received_headers)) = str;

	return size * nmemb;
}

static size_t
get_response_data(const void *buffer, const size_t size,
                  const size_t nmemb, const void *userp)
{
	char *str;

	/* libcurl doesn't guarantee NULL termination, so make sure ourselves. 
	 * We "leak" this since the pstrcat() below will allocate space as well,
	 * but it's not a huge amount of memory and auth only happens once.
	 * We could probably create a separate pool for this allocation, but
	 * it doesn't seem worthwhile for a few KiB at most.
	 */
	str = pstrndup(session.pool, buffer, size * nmemb);
	if (!str) {
		return 0;
	}

	response_data = pstrcat(session.pool, response_data, str, NULL);
	return size * nmemb;
}

static char *
urlencode(pool *p, const char *str)
{
	char *escaped, *check, *track;
	unsigned int num_to_escape;

	num_to_escape = 0;
	check = (char *) str;
	while (*check) {
		if (! (isalnum(*check) || *check == '-' || *check == '_' ||
		       *check == '.' || *check == ' ')) {

			++num_to_escape;
		}
		++check;
	}

	escaped = pcalloc(p, strlen(str) + (2 * num_to_escape) + 1);

	check = (char *) str;
	track = (char *) escaped;
	while (*check) {
		if (isalnum(*check) || *check == '-' || *check == '_' || *check == '.') {
			*track = *check;
		} else if (*check == ' ') {
			*track = '+';
		} else {
			char in_hex[3];
			snprintf(in_hex, 3, "%02x", *check);
			*track++ = '%';
			*track++ = in_hex[0];
			*track   = in_hex[1];
		}
		++track;
		++check;
	}
	*track = 0;

	return escaped;
}

MODRET
handle_auth_web_auth(cmd_rec *cmd)
{
	const char *username = cmd->argv[0];
	const char *password = cmd->argv[1];
	char *escaped_username, *escaped_password, *post_data,
		curl_error[CURL_ERROR_SIZE];
	unsigned int post_data_len;
	struct curl_slist *headers = NULL;
	CURL *curl_handle;
	CURLcode success;

	if (!url || !user_param_name || !pass_param_name || !local_user ||
	    !(failed_string || required_headers)) {
		return PR_DECLINED(cmd);
	}
	if (user_creg) {
		if (pr_regexp_exec(user_creg, cmd->argv[0], 0, NULL, 0, 0, 0) != 0) {
			pr_log_pri(PR_LOG_DEBUG, MOD_AUTH_WEB_VERSION ": user doesn't match regex");
			return PR_DECLINED(cmd);
		}
	}

	escaped_username = urlencode(cmd->tmp_pool, username);
	escaped_password = urlencode(cmd->tmp_pool, password);

	curl_handle = curl_easy_init();
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, curl_error);
	curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, get_response_headers);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, get_response_data);

	/* Not strictly necessary, but some sites arbitrarily block "spiders,"
	 * such as libcurl.
	 */
	headers = curl_slist_append(headers, pstrcat(cmd->tmp_pool, "User-Agent: ", MOD_AUTH_WEB_VERSION, NULL));
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

	/* user_param_name=escaped_username&pass_param_name=escaped_password\0 */
	post_data_len =
		strlen(user_param_name) + 1 + strlen(escaped_username) + 1 +
		strlen(pass_param_name) + 1 + strlen(escaped_password) + 1;
	post_data = pcalloc(session.pool, post_data_len);
	if (!post_data) {
		return PR_DECLINED(cmd);
	}
	snprintf(post_data, post_data_len, "%s=%s&%s=%s",
		user_param_name, escaped_username,
		pass_param_name, escaped_password);
	curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data);

	pr_log_pri(PR_LOG_DEBUG, MOD_AUTH_WEB_VERSION ": calling URL %s with POST data %s", url, post_data);
	success = curl_easy_perform(curl_handle);
	if (success == CURLE_OK) {
		pr_log_pri(PR_LOG_DEBUG, MOD_AUTH_WEB_VERSION ": URL call succeeded");
	} else {
		pr_log_pri(PR_LOG_ERR, MOD_AUTH_WEB_VERSION ": URL call failed: %s",
			curl_error);
		return PR_DECLINED(cmd);
	}

	if (failed_string && response_data && strstr(response_data, failed_string)) {
		pr_log_pri(PR_LOG_DEBUG, MOD_AUTH_WEB_VERSION ": found failed string '%s' in response", failed_string);
		return PR_ERROR_INT(cmd, PR_AUTH_BADPWD);
	}

	if (required_headers != NULL) {
		register unsigned int i, j;
		int found;
		char **required = (char **) required_headers->elts,
		     **received = (char **) received_headers->elts;

		for (i = 0; i < required_headers->nelts; ++i) {
			pr_log_pri(PR_LOG_DEBUG, MOD_AUTH_WEB_VERSION ": checking for header '%s' in response", required[i]);
			found = 0;

			for (j = 0; j < received_headers->nelts; ++j) {
				if (strcmp(required[i], received[j]) == 0) {
					found = 1;
					break;
				}
			}

			if (!found) {
				pr_log_pri(PR_LOG_DEBUG, MOD_AUTH_WEB_VERSION ": couldn't find header '%s' in response", required[i]);
				return PR_ERROR_INT(cmd, PR_AUTH_BADPWD);
			}
		}
	}

	session.auth_mech = "mod_auth_web.c";
	return PR_HANDLED(cmd);
}

MODRET
set_config_value(cmd_rec *cmd)
{
	CHECK_ARGS(cmd, 1);
	CHECK_CONF(cmd, CONF_ROOT | CONF_VIRTUAL | CONF_GLOBAL);

	add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
	return PR_HANDLED(cmd);
}

MODRET
set_user_regex(cmd_rec *cmd)
{
	pr_regex_t *creg;

	CHECK_ARGS(cmd, 1);
	CHECK_CONF(cmd, CONF_ROOT | CONF_VIRTUAL | CONF_GLOBAL);

	creg = pr_regexp_alloc(&auth_web_module);
	if (pr_regexp_compile_posix(creg, cmd->argv[1], REG_ICASE | REG_EXTENDED | REG_NOSUB) != 0) {
		CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, cmd->argv[0], ": unable to compile regex '", cmd->argv[1], "'", NULL));
	}
	add_config_param(cmd->argv[0], 1, (void *) creg);

	return PR_HANDLED(cmd);
}

static int
auth_web_getconf(void)
{
	config_rec *c;

	url = (char *) get_param_ptr(main_server->conf, "AuthWebURL", FALSE);
	user_param_name = (char *) get_param_ptr(main_server->conf,
		"AuthWebUsernameParamName", FALSE);
	pass_param_name = (char *) get_param_ptr(main_server->conf,
		"AuthWebPasswordParamName", FALSE);
	failed_string = (char *) get_param_ptr(main_server->conf,
		"AuthWebLoginFailedString", FALSE);
	local_user = (char *) get_param_ptr(main_server->conf,
		"AuthWebLocalUser", FALSE);
	user_creg = (pr_regex_t *) get_param_ptr(main_server->conf,
		"AuthWebUserRegex", FALSE);

	if ((c = find_config(main_server->conf, CONF_PARAM, "AuthWebRequireHeader", FALSE)) != NULL) {
		required_headers = make_array(session.pool, 1, sizeof(char *));
		do {
			*((char **) push_array(required_headers)) = c->argv[0];
		} while ((c = find_config_next(c, c->next, CONF_PARAM, "AuthWebRequireHeader", FALSE)));
	}

	return 0;
}

static conftable auth_web_config[] = {
	{ "AuthWebURL",               set_config_value, NULL },
	{ "AuthWebUsernameParamName", set_config_value, NULL },
	{ "AuthWebPasswordParamName", set_config_value, NULL },
	{ "AuthWebLoginFailedString", set_config_value, NULL },
	{ "AuthWebLocalUser",         set_config_value, NULL },
	{ "AuthWebRequireHeader",     set_config_value, NULL },
	{ "AuthWebUserRegex",         set_user_regex,   NULL },
	{ NULL,                       NULL,             NULL }
};

static authtable auth_web_auth[] = {
	{ 0, "getpwnam", handle_auth_web_getpwnam },
	{ 0, "auth",     handle_auth_web_auth     },
	{ 0, NULL }
};

module auth_web_module = {
	NULL, NULL,           /* Always NULL */
	0x20,                 /* Module API Version 2.0 */
	"auth_web",           /* Module name */
	auth_web_config,      /* Configuration handler table */
	NULL,                 /* Command handler table */
	auth_web_auth,        /* Authentication handler table */
	NULL,                 /* Module init function */
	auth_web_getconf,     /* Session init function */
	MOD_AUTH_WEB_VERSION  /* Module version */
};
