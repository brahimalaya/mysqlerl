/*
 * MySQL port driver.
 *
 * Copyright (C) 2008, Brian Cully <bjc@kublai.com>
 */

#include <erl_interface.h>
#include <ei.h>
#include <mysql.h>

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

const char *LOGPATH = "/tmp/mysqlerl.log";
static FILE *logfile = NULL;

const char *QUERY_MSG = "sql_query";
const char *COMMIT_MSG = "sql_commit";
const char *ROLLBACK_MSG = "sql_rollback";

typedef u_int32_t msglen_t;

struct msg {
  ETERM *cmd;
  unsigned char *buf;
};
typedef struct msg msg_t;

void
openlog()
{
  logfile = fopen(LOGPATH, "a");
}

void
closelog()
{
  fclose(logfile);
}

void
logmsg(const char *format, ...)
{
  FILE *out = logfile;
  va_list args;

  if (logfile == NULL)
    out = stderr;

  va_start(args, format);
  (void)vfprintf(out, format, args);
  (void)fprintf(out, "\n");
  va_end(args);

  fflush(out);
}

int
restartable_read(unsigned char *buf, size_t buflen)
{
  ssize_t rc, readb;

  rc = 0;
  READLOOP:
  while (rc < buflen) {
    readb = read(STDIN_FILENO, buf + rc, buflen - rc);
    if (readb == -1) {
      if (errno == EAGAIN || errno == EINTR)
        goto READLOOP;

      return -1;
    } else if (readb == 0) {
      logmsg("ERROR: EOF trying to read additional %d bytes from "
             "standard input", buflen - rc);
      return -1;
    }

    rc += readb;
  }

  return rc;
}

int
restartable_write(const unsigned char *buf, size_t buflen)
{
  ssize_t rc, wroteb;

  rc = 0;
  WRITELOOP:
  while (rc < buflen) {
    wroteb = write(STDOUT_FILENO, buf + rc, buflen - rc);
    if (wroteb == -1) {
      if (errno == EAGAIN || errno == EINTR)
        goto WRITELOOP;

      return -1;
    }

    rc += wroteb;
  }

  return rc;
}

msg_t *
read_msg()
{
  msg_t *msg;
  msglen_t len;

  msg = (msg_t *)malloc(sizeof(msg_t));
  if (msg == NULL) {
    logmsg("ERROR: Couldn't allocate message for reading: %s.\n",
           strerror(errno));

    exit(2);
  }

  logmsg("DEBUG: reading message length.");
  if (restartable_read((unsigned char *)&len, sizeof(len)) == -1) {
    logmsg("ERROR: couldn't read %d byte message prefix: %s.",
           sizeof(len), strerror(errno));

    free(msg);
    exit(2);
  }

  len = ntohl(len);
  msg->buf = malloc(len);
  if (msg->buf == NULL) {
    logmsg("ERROR: Couldn't malloc %d bytes: %s.", len,
           strerror(errno));

    free(msg);
    exit(2);
  }

  logmsg("DEBUG: reading message body (len: %d).", len);
  if (restartable_read(msg->buf, len) == -1) {
    logmsg("ERROR: couldn't read %d byte message: %s.",
           len, strerror(errno));

    free(msg->buf);
    free(msg);
    exit(2);
  }

  msg->cmd = erl_decode(msg->buf);

  return msg;
}

int
write_cmd(const char *cmd, msglen_t len)
{
  msglen_t nlen;

  nlen = htonl(len);
  if (restartable_write((unsigned char *)&nlen, sizeof(nlen)) == -1)
    return -1;
  if (restartable_write((unsigned char *)cmd, len) == -1)
    return -1;

  return 0;
}

void
handle_sql_query(MYSQL *dbh, ETERM *cmd)
{
  ETERM *query, *resp;
  char *q, *buf;
  int buflen;

  query = erl_element(2, cmd);
  q = erl_iolist_to_string(query);
  erl_free_term(query);

  logmsg("DEBUG: got query msg: %s.", q);
  if (mysql_query(dbh, q)) {
    resp = erl_format("{error, {mysql_error, ~i, ~s}}",
                      mysql_errno(dbh), mysql_error(dbh));
  } else {
    MYSQL_RES *result;

    result = mysql_store_result(dbh);
    if (result) {
      MYSQL_FIELD *fields;
      unsigned int i, num_fields, num_rows;
      ETERM **cols, *ecols, **rows, *erows;

      num_fields = mysql_num_fields(result);
      fields = mysql_fetch_fields(result);
      cols = (ETERM **)malloc(num_fields * sizeof(ETERM *));
      for (i = 0; i < num_fields; i++) {
        logmsg("DEBUG: cols[%d]: %s", i, fields[i].name);
        cols[i] = erl_format("~s", fields[i].name);
      }
      ecols = erl_mk_list(cols, num_fields);

      num_rows = mysql_num_rows(result);
      rows = (ETERM **)malloc(num_rows * sizeof(ETERM *));
      for (i = 0; i < num_rows; i++) {
        MYSQL_ROW row;
        ETERM **rowtup, *rt;
        unsigned int j;

        row = mysql_fetch_row(result);
        rowtup = (ETERM **)malloc(num_fields * sizeof(ETERM *));
        for (j = 0; j < num_fields; j++) {
          logmsg("DEBUG: rows[%d][%d]: %s", i, j, row[j]);
          rowtup[i] = erl_format("~s", row[j]);
        }
        rt = erl_mk_tuple(rowtup, num_fields);
        rows[i] = erl_format("~w", rt);

        for (j = 0; j < num_fields; j++)
          erl_free_term(rowtup[i]);
        free(rowtup);
        erl_free_term(rt);
      }
      erows = erl_mk_list(rows, num_rows);

      resp = erl_format("{selected, ~w, ~w}",
                        ecols, erows);

      for (i = 0; i < num_fields; i++)
        erl_free_term(cols[i]);
      free(cols);
      erl_free_term(ecols);

      for (i = 0; i < num_rows; i++)
        erl_free_term(rows[i]);
      free(rows);
      erl_free_term(erows);
    } else {
      if (mysql_field_count(dbh) == 0)
        resp = erl_format("{num_rows, ~i}", mysql_affected_rows(dbh));
      else
        resp = erl_format("{error, {mysql_error, ~i, ~s}}",
                          mysql_errno(dbh), mysql_error(dbh));
    }
  }
  erl_free(q);

  buflen = erl_term_len(resp);
  buf = (char *)malloc(buflen);
  erl_encode(resp, (unsigned char *)buf);
  erl_free_term(resp);
  write_cmd(buf, buflen);
  free(buf);
}

void
handle_sql_commit(MYSQL *dbh, ETERM *cmd)
{
  ETERM *resp;
  char *buf;
  int buflen;

  resp = erl_format("{ok, commit}");
  buflen = erl_term_len(resp);
  buf = (char *)malloc(buflen);
  erl_encode(resp, (unsigned char *)buf);
  erl_free_term(resp);
  write_cmd(buf, buflen);
  free(buf);
}

void
handle_sql_rollback(MYSQL *dbh, ETERM *cmd)
{
  ETERM *resp;
  char *buf;
  int buflen;

  resp = erl_format("{ok, rollback}");
  buflen = erl_term_len(resp);
  buf = (char *)malloc(buflen);
  erl_encode(resp, (unsigned char *)buf);
  erl_free_term(resp);
  write_cmd(buf, buflen);
  free(buf);
}

void
dispatch_db_cmd(MYSQL *dbh, msg_t *msg)
{
  ETERM *tag;

  tag = erl_element(1, msg->cmd);
  if (strncmp((char *)ERL_ATOM_PTR(tag),
              QUERY_MSG, strlen(QUERY_MSG)) == 0) {
    handle_sql_query(dbh, msg->cmd);
  } else if (strncmp((char *)ERL_ATOM_PTR(tag),
                     COMMIT_MSG, strlen(COMMIT_MSG)) == 0) {
    handle_sql_commit(dbh, msg->cmd);
  } else if (strncmp((char *)ERL_ATOM_PTR(tag),
                     ROLLBACK_MSG, strlen(ROLLBACK_MSG)) == 0) {
    handle_sql_rollback(dbh, msg->cmd);
  } else {
    logmsg("WARNING: message type %s unknown.", (char *)ERL_ATOM_PTR(tag));
    erl_free_term(tag);
    exit(3);
  }

  erl_free_term(tag);
}

void
usage()
{
  fprintf(stderr, "Usage: mysqlerl host port db_name user passwd\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  MYSQL dbh;
  char *host, *port, *db_name, *user, *passwd;
  msg_t *msg;

  openlog();
  logmsg("INFO: starting up.");

  if (argc < 6)
    usage();

  host    = argv[1];
  port    = argv[2];
  db_name = argv[3];
  user    = argv[4];
  passwd  = argv[5];

  erl_init(NULL, 0);

  mysql_init(&dbh);
  if (mysql_real_connect(&dbh, host, user, passwd,
                         db_name, atoi(port), NULL, 0) == NULL) {
    logmsg("ERROR: Failed to connect to database %s: %s (%s:%s).",
           db_name, mysql_error(&dbh), user, passwd);
    exit(2);
  }

  while ((msg = read_msg()) != NULL) {
    dispatch_db_cmd(&dbh, msg);

    /* XXX: Move this to function */
    erl_free_compound(msg->cmd);
    free(msg->buf);
    free(msg);
  }

  mysql_close(&dbh);

  logmsg("INFO: shutting down.");
  closelog();

  return 0;
}
