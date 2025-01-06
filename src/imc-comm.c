/*
 * IMC2 - an inter-mud communications protocol
 *
 * imc-comm.c: command interface code
 *
 * Copyright (C) 1996 Oliver Jowett <oliver@sa-search.massey.ac.nz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

/*  USEIOCTL #defined if TIOCINQ or TIOCOUTQ are - we assume that the ioctls
 *  work in that case.
 */

#if defined(TIOCINQ) && defined(TIOCOUTQ)
#define USEIOCTL
static int outqsize;
#endif

#include "imc.h"
#include "imc-comm.h"
#include "imc-mail.h"

/* rignore'd people/muds - should be a list, really */
char *imc_rignore[IMC_RIGNORE_MAX];
/* prefixes for all data files */
char *imc_prefix;

/* called when a keepalive has been received */
void imc_recv_keepalive(const char *from, const char *version)
{
  imc_reminfo *p;

  if (!strcasecmp(from, imc_name))
    return;

  p=imc_find_reminfo(from); /*  this should never fail, imc.c should create an
			     *  entry if one doesn't exist (in the path update
			     *  code)
			     */
  if (!p)		    /* boggle */
    return;

  /* lower-level code has already updated p->alive */

  if (strcasecmp(version, p->version))    /* remote version has changed? */
  {
    imc_strfree(p->version);              /* if so, update it */
    p->version=imc_strdup(version);
  }
}

/* called when a ping request is received */
void imc_recv_ping(const char *from, int time_s, int time_u)
{
  /* ping 'em back */
  imc_send_pingreply(from, time_s, time_u);
}

/* called when a ping reply is received */
void imc_recv_pingreply(const char *from, int time_s, int time_u)
{
  imc_reminfo *p;
  struct timeval tv;

  p = imc_find_reminfo(from);   /* should always exist */
  if (!p)			/* boggle */
    return;

  gettimeofday(&tv, NULL);      /* grab the exact time now and calc RTT */
  p->ping = (tv.tv_sec - time_s) * 1000 + (tv.tv_usec - time_u) / 1000;
}

/* check if a packet from a given source should be ignored */
int imc_isignored(const char *who)
{
  int i;

  for (i=0; i<IMC_RIGNORE_MAX; i++)
    if (imc_rignore[i] &&
	(!strcasecmp(imc_rignore[i], who) ||
	 !strcasecmp(imc_rignore[i], imc_mudof(who))))
      return 1;

  return 0;
}

/* send a standard 'you are being ignored' rtell */
static void sendignore(const char *to)
{
  char buf[IMC_DATA_LENGTH];

  sprintf(buf, "%s is ignoring you", imc_name);
  imc_send_tell(NULL, to, buf, 1);
}

/** imc_char_data representation:
 *
 *  All levels are "MAX_LEVEL-independent". 0 and up are mortal levels as
 *  usual. -1 is MAX_LEVEL. -2 is MAX_LEVEL-1, and so on. Conversion between
 *  imc_char_data and the mud's internal levels etc. is done in the interface
 *  file (eg. imc-rom.c or imc-envy.c)
 *
 *  Invis/hidden state, and detecting them, use the IMC_INVIS and IMC_HIDDEN
 *  #defines.
 *
 *  d->wizi  is the wizi or incog level of the character (whichever is higher)
 *  d->invis is a bitvector representing the invis/hidden state of a character
 *  d->see   is a bitvector representing detect invis/hidden for a character
 *  d->level is the level of the character (for the purposes of seeing
 *           wizi/incog)
 *  d->sex   is currently unused, but will eventually be used for socials
 */

/* convert from the char data in 'p' to an internal representation in 'd' */
static void getdata(const imc_packet *p, imc_char_data *d)
{
  strcpy(d->name, p->from);
  d->invis = imc_getkeyi(&p->data, "invis", 0);
  d->wizi  = imc_getkeyi(&p->data, "wizi", 0);
  d->see   = imc_getkeyi(&p->data, "see", 0);
  d->level = imc_getkeyi(&p->data, "level", 0);
  d->sex   = imc_getkeyi(&p->data, "sex", 0);
}

/* convert back from 'd' to 'p' */
static void setdata(imc_packet *p, const imc_char_data *d)
{
  imc_initdata(&p->data);

  if (!d)
  {
    strcpy(p->from, "*");
    imc_addkeyi(&p->data, "level", -1);
    return;
  }

  strcpy(p->from, d->name);

  if (d->invis)
    imc_addkeyi(&p->data, "invis", d->invis);
  if (d->wizi)
    imc_addkeyi(&p->data, "wizi", d->wizi);
  if (d->see)
    imc_addkeyi(&p->data, "see", d->see);
  if (d->level)
    imc_addkeyi(&p->data, "level", d->level);
  if (d->sex)
    imc_addkeyi(&p->data, "sex", d->sex);
}

/* handle a packet destined for us, or a broadcast */
void imc_recv(const imc_packet *p)
{
  imc_char_data d;

  getdata(p, &d);

  /* chat: message to a channel (broadcast) */
  if (!strcasecmp(p->type, "chat") && !imc_isignored(p->from))
    imc_recv_chat(&d, imc_getkeyi(&p->data, "channel", 0),
		  imc_getkey(&p->data, "text", ""));

  /* emote: emote to a channel (broadcast) */
  else if (!strcasecmp(p->type, "emote") && !imc_isignored(p->from))
    imc_recv_emote(&d, imc_getkeyi(&p->data, "channel", 0),
		   imc_getkey(&p->data, "text", ""));

  /* tell: tell a player here something */
  else if (!strcasecmp(p->type, "tell"))
    if (imc_isignored(p->from))
      sendignore(p->from);
    else
      imc_recv_tell(&d, p->to, imc_getkey(&p->data, "text", ""),
		    imc_getkeyi(&p->data, "isreply", 0));

  /* who-reply: receive a who response */
  else if (!strcasecmp(p->type, "who-reply"))
    imc_recv_whoreply(p->to, imc_getkey(&p->data, "text", ""));

  /* who: receive a who request */
  else if (!strcasecmp(p->type, "who"))
    if (imc_isignored(p->from))
      sendignore(p->from);
    else
      imc_recv_who(&d, imc_getkey(&p->data, "type", "who"));

  /* beep: beep a player */
  else if (!strcasecmp(p->type, "beep"))
    if (imc_isignored(p->from))
      sendignore(p->from);
    else
      imc_recv_beep(&d, p->to);

  /* is-alive: receive a keepalive (broadcast) */
  else if (!strcasecmp(p->type, "is-alive"))
    imc_recv_keepalive(imc_mudof(p->from),
		       imc_getkey(&p->data, "versionid", "unknown"));

  /* ping: receive a ping request */
  else if (!strcasecmp(p->type, "ping"))
    imc_recv_ping(imc_mudof(p->from), imc_getkeyi(&p->data, "time-s", 0),
		  imc_getkeyi(&p->data, "time-us", 0));

  /* ping-reply: receive a ping reply */
  else if (!strcasecmp(p->type, "ping-reply"))
    imc_recv_pingreply(imc_mudof(p->from), imc_getkeyi(&p->data, "time-s", 0),
		       imc_getkeyi(&p->data, "time-us", 0));

  /* mail: mail something to a local player */
  else if (!strcasecmp(p->type, "mail"))
    imc_recv_mail(imc_getkey(&p->data, "from", "error@hell"),
		  imc_getkey(&p->data, "to", "error@hell"),
		  imc_getkey(&p->data, "date", "(IMC error: bad date)"),
		  imc_getkey(&p->data, "subject", "no subject"),
		  imc_getkey(&p->data, "id", "bad_id"),
		  imc_getkey(&p->data, "text", ""));

  /* mail-ok: remote confirmed that they got the mail ok */
  else if (!strcasecmp(p->type, "mail-ok"))
    imc_recv_mailok(p->from, imc_getkey(&p->data, "id", "bad_id"));

  /* mail-reject: remote rejected our mail, bounce it */
  else if (!strcasecmp(p->type, "mail-reject"))
    imc_recv_mailrej(p->from, imc_getkey(&p->data, "id", "bad_id"),
		     imc_getkey(&p->data, "reason",
				"(IMC error: no reason supplied"));
}


/* Commands called by the interface layer */

/* send a message out on a channel */
void imc_send_chat(const imc_char_data *from, int channel,
		   const char *argument, const char *to)
{
  imc_packet out;
  char tobuf[IMC_MNAME_LENGTH];

  setdata(&out, from);

  strcpy(out.type, "chat");
  strcpy(out.to, "*@*");
  imc_addkey(&out.data, "text", argument);
  imc_addkeyi(&out.data, "channel", channel);

  to=imc_getarg(to, tobuf, IMC_MNAME_LENGTH);
  while (tobuf[0])
  {
  if (!strcmp(tobuf, "*")
  || !strcasecmp(tobuf, imc_name)
  || imc_find_reminfo(tobuf)!=NULL)  
  {
    strcpy(out.to, "*@");
    strcat(out.to, tobuf);
    imc_send(&out);
  }

    to=imc_getarg(to, tobuf, IMC_MNAME_LENGTH);
  }

  imc_freedata(&out.data);
}

/* send an emote out on a channel */
void imc_send_emote(const imc_char_data *from, int channel,
		    const char *argument, const char *to)
{
  imc_packet out;
  char tobuf[IMC_MNAME_LENGTH];

  setdata(&out, from);

  strcpy(out.type, "emote");
  imc_addkeyi(&out.data, "channel", channel);
  imc_addkey(&out.data, "text", argument);

  to=imc_getarg(to, tobuf, IMC_MNAME_LENGTH);
  while (tobuf[0])
  {
  if (!strcmp(tobuf, "*")
  || !strcasecmp(tobuf, imc_name)
  || imc_find_reminfo(tobuf)!=NULL)  
  {
    strcpy(out.to, "*@");
    strcat(out.to, tobuf);
    imc_send(&out);
  }

    to=imc_getarg(to, tobuf, IMC_MNAME_LENGTH);
  }

  imc_freedata(&out.data);
}

/* send a tell to a remote player */
void imc_send_tell(const imc_char_data *from, const char *to,
		   const char *argument, int isreply)
{
  imc_packet out;

  setdata(&out, from);

  imc_sncpy(out.to, to, IMC_NAME_LENGTH);
  strcpy(out.type, "tell");
  imc_addkey(&out.data, "text", argument);
  if (isreply)
    imc_addkeyi(&out.data, "isreply", isreply);

  imc_send(&out);
  imc_freedata(&out.data);
}

/* send a who-request to a remote mud */
void imc_send_who(const imc_char_data *from, const char *to, const char *type)
{
  imc_packet out;

  setdata(&out, from);

  sprintf(out.to, "*@%s", to);
  strcpy(out.type, "who");

  imc_addkey(&out.data, "type", type);

  imc_send(&out);
  imc_freedata(&out.data);
}

/* respond to a who request with the given data */
void imc_send_whoreply(const char *to, const char *data)
{
  imc_packet out;

  imc_initdata(&out.data);

  imc_sncpy(out.to, to, IMC_NAME_LENGTH);
  strcpy(out.type, "who-reply");
  strcpy(out.from, "*");
  imc_addkey(&out.data, "text", data);

  imc_send(&out);
  imc_freedata(&out.data);
}

/* beep a remote player */
void imc_send_beep(const imc_char_data *from, const char *to)
{
  imc_packet out;

  setdata(&out, from);
  strcpy(out.type, "beep");
  imc_sncpy(out.to, to, IMC_NAME_LENGTH);

  imc_send(&out);
  imc_freedata(&out.data);
}

/* send a keepalive to everyone */
void imc_send_keepalive(void)
{
  imc_packet out;

  imc_initdata(&out.data);
  strcpy(out.type, "is-alive");
  strcpy(out.from, "*");
  strcpy(out.to, "*@*");
  imc_addkey(&out.data, "versionid", IMC_VERSIONID);

  imc_send(&out);
  imc_freedata(&out.data);
}

/* send a ping with a given timestamp */
void imc_send_ping(const char *to, int time_s, int time_u)
{
  imc_packet out;

  imc_initdata(&out.data);
  strcpy(out.type, "ping");
  strcpy(out.from, "*");
  strcpy(out.to, "*@");
  imc_sncpy(out.to+2, to, IMC_MNAME_LENGTH-2);
  imc_addkeyi(&out.data, "time-s", time_s);
  imc_addkeyi(&out.data, "time-us", time_u);

  imc_send(&out);
  imc_freedata(&out.data);
}

/* send a pingreply with the given timestamp */
void imc_send_pingreply(const char *to, int time_s, int time_u)
{
  imc_packet out;

  imc_initdata(&out.data);
  strcpy(out.type, "ping-reply");
  strcpy(out.from, "*");
  strcpy(out.to, "*@");
  imc_sncpy(out.to+2, to, IMC_MNAME_LENGTH-2);
  imc_addkeyi(&out.data, "time-s", time_s);
  imc_addkeyi(&out.data, "time-us", time_u);

  imc_send(&out);
  imc_freedata(&out.data);
}

/* admin commands */

/* add/remove/list rignores */
const char *imc_ignore(const char *what)
{
  static char buf[IMC_DATA_LENGTH];
  int i, count;

  if (!what || !what[0])
  {
    strcpy(buf, "Current ignores:\n\r");
    for (i=0, count=0; i<IMC_RIGNORE_MAX; i++)
      if (imc_rignore[i])
      {
	sprintf(buf + strlen(buf), " %s\n\r", imc_rignore[i]);
	count++;
      }

    if (!count)
      strcat(buf, " none");
    else
      sprintf(buf + strlen(buf), "[total %d]", count);

    return buf;
  }

  for (i=0; i<IMC_RIGNORE_MAX; i++)
    if (imc_rignore[i] && !strcasecmp(what, imc_rignore[i]))
    {
      imc_strfree(imc_rignore[i]);
      imc_rignore[i] = NULL;
      imc_saveignores();
      return "Ignore removed";
    }

  for (i=0; i<IMC_RIGNORE_MAX; i++)
    if (!imc_rignore[i])
    {
      imc_rignore[i] = imc_strdup(what);
      imc_saveignores();
      return "Ignore added";
    }

  return "No ignore slots free";
}

/* show current IMC socket states */
const char *imc_sockets(void)
{
  int i;
  static char buf[IMC_DATA_LENGTH];
  char *state;
  int r, s;

  sprintf(buf, "%2s %4s %-9s %-15s %-6s %-6s %-6s %-6s",
	  "# ", "Desc", "Mud", "State", "Inbuf", "Outbuf", "Spam1", "Spam2");

  for (i=0; i<IMC_MAX; i++)
  {
    if (imc[i].inuse)
    {
      switch (imc[i].state)
      {
      case IMC_CONNECTING:
	state = "connecting";
	break;
      case IMC_WAIT1:
	state = "wait1";
	break;
      case IMC_WAIT2:
	state = "wait2";
	break;
      case IMC_CONNECTED:
	state = "connected";
	break;
      default:
	state = "unknown";
	break;
      }

#ifdef USEIOCTL
      /* try to work out the system buffer sizes */
      r=0;
      ioctl(imc[i].desc, TIOCINQ, &r);
      r += strlen(imc[i].inbuf);

      s=outqsize;
      if (s)
      {
	ioctl(imc[i].desc, TIOCOUTQ, &s);
	s=outqsize-s;
      }
      s += strlen(imc[i].outbuf);
#else
      r=strlen(imc[i].inbuf);
      s=strlen(imc[i].outbuf);
#endif

      sprintf(buf + strlen(buf), "\n\r%2d %4d %-9s %-15s %6d %6d %6d %6d",
	      i,
	      imc[i].desc,
	      imc[i].info != -1 ? imc_info[imc[i].info].name : "unknown",
	      state,
	      r,
	      s,
              imc[i].spamcounter1,
              imc[i].spamcounter2);
    }
  }

  return buf;
}

/*  list current connections/known muds
 *  level=0 is mortal-level access (mudnames and connection states)
 *  level=1 is imm-level access (names, hosts, ports, states)
 *  level=2 is full access (names, hosts, ports, passwords, flags, states)
 */
const char *imc_list(int level)
{
  int i;
  static char buf[IMC_DATA_LENGTH];
  char *state;
  imc_reminfo *p;

  strcpy(buf, "Direct connections:\n\r");

  switch (level)
  {
  case 0:
    sprintf(buf + strlen(buf), "%-10s %-15s", "Name", "State");
    break;
  case 1:
    sprintf(buf + strlen(buf), "%-10s %-30s %5s %-13s", "Name", "Host", "Port",
	    "State");
    break;
  case 2:
    sprintf(buf + strlen(buf),
	    "%-8s %-25s %5s %-13s %-10s %-10s\n"
	    "         %-8s %-9s %s",
	    "Name", "Host", "Port", "State", "ClientPW", "ServerPW",
	    "RcvStamp", "NoForward", "Flags");
    break;
  }

  for (i=0; i<IMC_MAX; i++)
  {
    if (!imc_info[i].inuse)
      continue;

    state = imc_info[i].connected ? "connected" : "not connected";

    switch (level)
    {
    case 0:
      sprintf(buf + strlen(buf), "\n\r%-10s %-15s", imc_info[i].name, state);
      break;
    case 1:
      sprintf(buf + strlen(buf), "\n\r%-10s %-30s %5hu %-13s",
	      imc_info[i].name,
	      imc_info[i].host,
	      imc_info[i].port,
	      state);
      break;
    case 2:
      sprintf(buf + strlen(buf),
	      "\n\r%-8s %-25s %5hu %-13s %-10s %-10s"
	      "\n\r         %-8d %-9d %s",
	      imc_info[i].name,
	      imc_info[i].host,
	      imc_info[i].port,
	      state,
	      imc_info[i].clientpw,
	      imc_info[i].serverpw,
	      imc_info[i].rcvstamp,
	      imc_info[i].noforward,
	      imc_flagname(imc_info[i].flags));
      break;
    }
  }

  strcpy(buf + strlen(buf), "\n\r\n\rActive muds on IMC:\n\r");
  sprintf(buf + strlen(buf), "%-10s  %-10s  %-9s  %-20s  %-10s", "Name",
	  "Last alive", "Ping time", "IMC Version", "Route");

  for (p=imc_remoteinfo; p; p=p->next)
    if (p->ping)
      sprintf(buf + strlen(buf), "\n\r%-10s  %9ds  %7dms  %-20s  %-10s",
	      p->name, (int) (imc_now - p->alive), p->ping, p->version,
	      p->route ? p->route : "broadcast");
    else
      sprintf(buf + strlen(buf), "\n\r%-10s  %9ds  %9s  %-20s  %-10s",
	      p->name, (int) (imc_now - p->alive), "unknown", p->version,
	      p->route ? p->route : "broadcast");

  return buf;
}

/*  runtime changing of imc.conf
 *  returns  >0 success
 *           <0 error
 *          ==0 unknown command
 *
 *  commands:
 *    add <mudname>
 *    delete <mudname>
 *    rename <oldname> <newname>
 *    set <mudname> <host|port|clientpw|serverpw|flags> <newvalue>
 *    set <mudname> all <host> <port> <clientpw> <serverpw> <flags>
 */

int imc_command(const char *argument)
{
  char arg1[IMC_DATA_LENGTH];
  char arg2[IMC_DATA_LENGTH];
  char arg3[IMC_DATA_LENGTH];
  int i;

  argument=imc_getarg(argument, arg1, IMC_DATA_LENGTH);
  argument=imc_getarg(argument, arg2, IMC_DATA_LENGTH);

  if (!arg1[0] || !arg2[0])
    return 0;

  if (!strcasecmp(arg1, "add"))
  {
    for (i=0; i<IMC_MAX; i++)
      if (!imc_info[i].inuse)
	break;

    if (i == IMC_MAX)
    {
      imc_qerror("No more entries are available");
      return -1;
    }

    imc_info[i].name      = imc_strdup(arg2);
    imc_info[i].host      = imc_strdup("");
    imc_info[i].port      = 0;
    imc_info[i].connected = 0;
    imc_info[i].index     = -1;
    imc_info[i].clientpw  = imc_strdup("");
    imc_info[i].serverpw  = imc_strdup("");
    imc_info[i].timer     = 0;
    imc_info[i].inuse     = 1;
    imc_info[i].rcvstamp  = 0;
    imc_info[i].noforward = 0;

    return 1;
  }
  else if (!strcasecmp(arg1, "delete"))
  {
    i=imc_getindex(arg2);

    if (i == -1)
    {
      imc_qerror("Entry not found");
      return -1;
    }

    imc_disconnect(arg2);

    imc_strfree(imc_info[i].name);
    imc_strfree(imc_info[i].host);
    imc_strfree(imc_info[i].clientpw);
    imc_strfree(imc_info[i].serverpw);
    imc_info[i].inuse = 0;

    imc_saveconfig();
    return 1;
  }
  else if (!strcasecmp(arg1, "rename"))
  {
    i=imc_getindex(arg2);

    if (i == -1)
    {
      imc_qerror("Entry not found");
      return -1;
    }

    argument=imc_getarg(argument, arg3, IMC_DATA_LENGTH);
    if (!arg3[0])
      return 0;

    imc_strfree(imc_info[i].name);
    imc_info[i].name = imc_strdup(arg3);

    imc_saveconfig();
    return 1;
  }
  else if (!strcasecmp(arg1, "set"))
  {
    for (i=0; i<IMC_MAX; i++)
      if (imc_info[i].inuse && !strcasecmp(arg2, imc_info[i].name))
	break;

    if (i == IMC_MAX)
    {
      imc_qerror("Entry not found");
      return -1;
    }

    argument=imc_getarg(argument, arg3, IMC_DATA_LENGTH);

    if (!arg3[0] || !argument[0])
      return 0;
    else if (!strcasecmp(arg3, "all"))
    {
      imc_strfree(imc_info[i].host);
      imc_strfree(imc_info[i].clientpw);
      imc_strfree(imc_info[i].serverpw);

      argument=imc_getarg(argument, arg3, IMC_DATA_LENGTH);
      imc_info[i].host=imc_strdup(arg3);
      argument=imc_getarg(argument, arg3, IMC_DATA_LENGTH);
      imc_info[i].port=strtoul(arg3, NULL, 10);
      argument=imc_getarg(argument, arg3, IMC_PW_LENGTH);
      imc_info[i].clientpw=imc_strdup(arg3);
      argument=imc_getarg(argument, arg3, IMC_PW_LENGTH);
      imc_info[i].serverpw=imc_strdup(arg3);
      argument=imc_getarg(argument, arg3, IMC_DATA_LENGTH);
      imc_info[i].rcvstamp=strtoul(arg3, NULL, 10);
      argument=imc_getarg(argument, arg3, IMC_DATA_LENGTH);
      imc_info[i].noforward=strtoul(arg3, NULL, 10);
      argument=imc_getarg(argument, arg3, IMC_DATA_LENGTH);
      imc_info[i].flags=imc_flagvalue(arg3);

      imc_saveconfig();

      return 1;
    }
    else if (!strcasecmp(arg3, "host"))
    {
      imc_strfree(imc_info[i].host);
      imc_info[i].host=imc_strdup(argument);

      imc_saveconfig();
      return 1;
    }
    else if (!strcasecmp(arg3, "port"))
    {
      imc_info[i].port=strtoul(argument, NULL, 10);

      imc_saveconfig();
      return 1;
    }
    else if (!strcasecmp(arg3, "clientpw"))
    {
      imc_strfree(imc_info[i].clientpw);
      imc_info[i].clientpw=imc_strdup(argument);

      imc_saveconfig();
      return 1;
    }
    else if (!strcasecmp(arg3, "serverpw"))
    {
      imc_strfree(imc_info[i].serverpw);
      imc_info[i].serverpw=imc_strdup(argument);

      imc_saveconfig();
      return 1;
    }
    else if (!strcasecmp(arg3, "rcvstamp"))
    {
      imc_info[i].rcvstamp=strtoul(argument, NULL, 10);

      imc_saveconfig();
      return 1;
    }
    else if (!strcasecmp(arg3, "noforward"))
    {
      imc_info[i].noforward=strtoul(argument, NULL, 10);

      imc_saveconfig();
      return 1;
    }
    else if (!strcasecmp(arg3, "flags"))
    {
      imc_info[i].flags=imc_flagvalue(argument);

      imc_saveconfig();
      return 1;
    }

    return 0;
  }

  return 0;
}

/* get some IMC stats, return a string describing them */
const char *imc_getstats(void)
{
  static char buf[300];

  sprintf(buf,
	  "IMC statistics\n\r"
	  "\n\r"
	  "Received packets:    %ld\n\r"
	  "Received bytes:      %ld (%ld/second)\n\r"
	  "Transmitted packets: %ld\n\r"
	  "Transmitted bytes:   %ld (%ld/second)\n\r",

	  imc_stats.rx_pkts,
	  imc_stats.rx_bytes,
	  imc_stats.rx_bytes /
	  ((imc_now - imc_stats.start) ?
	   (imc_now - imc_stats.start) : 1),
	  imc_stats.tx_pkts,
	  imc_stats.tx_bytes,
	  imc_stats.tx_bytes /
	  ((imc_now - imc_stats.start) ?
	   (imc_now - imc_stats.start) : 1));

  return buf;
}

/* read an IMC config file */
int imc_readconfig(void)
{
  FILE *cf;
  int i;
  char name[IMC_NAME_LENGTH], host[200];
  char pw1[IMC_PW_LENGTH], pw2[IMC_PW_LENGTH];
  unsigned short port;
  int count, count1;
  char buf[1000];
  char configfile[200];

  imc_sncpy(configfile, imc_prefix, 193);
  strcat(configfile, "config");

  for (i=0; i<IMC_MAX; i++)
  {
    imc[i].inuse      = 0;
    imc_info[i].inuse = 0;
  }

  cf=fopen(configfile, "r");
  if (!cf)
  {
    imc_logerror("imc_readconfig: couldn't open %s", configfile);
    return 0;
  }

  for (i=0; i<IMC_MAX;)
  {
    if (fgets(buf, 1000, cf) == NULL)
      break;

    if (buf[0] == '#' || buf[0] == '\n')
      continue;

    if (sscanf(buf, "%s %s %hu %s %s %n",
	       name, host, &port, pw1, pw2, &count) < 5)
    {
      imc_logerror("Bad config file line: %s", buf);
      i--;
      continue;
    }

    imc_info[i].name      = imc_strdup(name);
    imc_info[i].host      = imc_strdup(host);
    imc_info[i].clientpw  = imc_strdup(pw1);
    imc_info[i].serverpw  = imc_strdup(pw2);
    imc_info[i].port      = port;
    imc_info[i].connected = 0;
    imc_info[i].index     = -1;
    imc_info[i].inuse     = 1;

    if (sscanf(buf+count, "%d %d %n",
	       &imc_info[i].rcvstamp,
	       &imc_info[i].noforward,
	       &count1)<2)
    { /* old version config file */
      imc_info[i].rcvstamp  = 0;
      imc_info[i].noforward = 0;
      imc_info[i].flags     = imc_flagvalue(buf + count);
    }
    else
      imc_info[i].flags     = imc_flagvalue(buf + count + count1);

    i++;
  }

  if (ferror(cf))
  {
    imc_lerror("imc_readconfig");
    fclose(cf);
    return 0;
  }

  fclose(cf);
  return 1;
}

/* save the IMC config file (under whatever name it was loaded from) */
int imc_saveconfig(void)
{
  FILE *out;
  int i;
  char configfile[200];

  imc_sncpy(configfile, imc_prefix, 193);
  strcat(configfile, "config");

  out = fopen(configfile, "w");
  if (!out)
  {
    imc_lerror("imc_saveconfig: error opening %s", configfile);
    return 0;
  }

  fprintf(out, "%-10s %-30s %5s %-10s %-10s %-5s %-5s %s\n",
	  "# Name", "Host", "Port", "ClientPW", "ServerPW",
	  "RcvSt", "NoFwd", "Flags");

  for (i=0; i<IMC_MAX; i++)
    if (imc_info[i].inuse)
      fprintf(out, "%-10s %-30s %5hu %-10s %-10s %5d %5d %s\n",
	      imc_info[i].name,
	      imc_info[i].host,
	      imc_info[i].port,
	      imc_info[i].clientpw,
	      imc_info[i].serverpw,
	      imc_info[i].rcvstamp,
	      imc_info[i].noforward,
	      imc_flagname(imc_info[i].flags));

  if (ferror(out))
  {
    imc_lerror("imc_saveconfig: error saving %s", configfile);
    fclose(out);
    return 0;
  }

  fclose(out);
  return 1;
}

/* read an IMC rignores file */
int imc_readignores(void)
{
  FILE *inf;
  int i;
  char buf[1000];
  char buf1[IMC_NAME_LENGTH];
  char name[200];

  imc_sncpy(name, imc_prefix, 191);
  strcat(name, "rignores");

  for (i=0; i<IMC_RIGNORE_MAX; i++)
    imc_rignore[i]=NULL;

  inf=fopen(name, "r");
  if (!inf)
  {
    imc_logerror("imc_readignores: couldn't open %s", name);
    return 0;
  }

  for (i=0; i<IMC_RIGNORE_MAX;)
  {
    if (fgets(buf, 1000, inf) == NULL)
      break;

    if (buf[0] == '#' || buf[0] == '\n')
      continue;

    sscanf(buf, "%[^\n]", buf1);
    imc_rignore[i]=imc_strdup(buf1);
    i++;
  }

  if (ferror(inf))
  {
    imc_lerror("imc_readignores");
    fclose(inf);
    return 0;
  }

  fclose(inf);
  return 1;
}

/* save the current rignore list */
int imc_saveignores(void)
{
  FILE *out;
  int i;
  char name[200];

  imc_sncpy(name, imc_prefix, 191);
  strcat(name, "rignores");

  out = fopen(name, "w");
  if (!out)
  {
    imc_lerror("imc_saveignores: error opening %s", name);
    return 0;
  }

  fprintf(out,
	  "# IMC rignores file, one name per line, no leading spaces\n\r"
	  "# lines starting with '#' are discarded\n\r");

  for (i=0; i<IMC_RIGNORE_MAX; i++)
    if (imc_rignore[i])
      fprintf(out, "%s\n", imc_rignore[i]);

  if (ferror(out))
  {
    imc_lerror("imc_saveignores: error saving %s", name);
    fclose(out);
    return 0;
  }

  fclose(out);
  return 1;
}

#ifdef USEIOCTL
/*  this is an ugly hack to generate the send-queue size for an empty queue.
 *  SO_SNDBUF is only supported in some places, and seems to cause problems
 *  under SunOS
 */

/*  connect to the local discard server, and look at the queue size for an
 *  empty socket.
 */
static int getsndbuf(void)
{
  struct sockaddr_in sa;
  int s, queue;

  if ((s=socket(AF_INET, SOCK_STREAM, 0))<0)
    return 0;

  sa.sin_family      = AF_INET;
  sa.sin_addr.s_addr = inet_addr("127.0.0.1");	/* connect to localhost */
  sa.sin_port        = htons(9);                /* 'discard' service */

  if (connect(s, (struct sockaddr *)&sa, sizeof(sa))<0)
  {
    close(s);
    return 0;
  }

  if (ioctl(s, TIOCOUTQ, &queue)<0)
  {
    close(s);
    return 0;
  }

  close(s);
  return queue;
}
#endif

/*  start everything up
 *  'name' is the mudname of this mud (can't contain spaces, and should be
 *         short).
 *  'port' is the port to listen on for incoming connections.
 *  'prefix' is the prefix to add to all IMC file references. For example,
 *           you could pass "imc/" to get imc/config, imc/mail-queue, etc. 
 */

int imc_startup(const char *name, int port, const char *prefix)
{
#ifdef USEIOCTL
  outqsize = getsndbuf();
  imc_logstring("Found TIOCOUTQ=%d", outqsize);
#endif

  imc_now=time(NULL);                  /* start our clock */
  imc_prefix=imc_strdup(prefix);

  imc_readconfig();                    /* ignore errors */
  imc_readignores();

  if (!imc_ll_startup(name, port))
    return 0;

  imc_remoteinfo=NULL;

  imc_mail_startup();		/* start up the mailer */

  return 1;
}

void imc_idle(void)
{
  static time_t nextalive;
  static time_t nextping;
  static int whichping;
  imc_reminfo *p, *pnext;

  imc_now=time(NULL);

  /* keepalives and time out old reminfo entries */

  if (!nextalive)		/* on startup */
    nextalive=imc_now + 60;

  if (nextalive<imc_now)
  {
    imc_send_keepalive();
    nextalive=imc_now+IMC_KEEPALIVE_TIME;

    for (p=imc_remoteinfo; p; p=pnext)
    {
      pnext=p->next;
      if (p->alive+IMC_KEEPALIVE_TIMEOUT < imc_now)
	imc_delete_reminfo(p);
    }
  }

  /* send pings */

  if (nextping<imc_now)
  {
    int i;
    struct timeval tv;

    nextping=imc_now+IMC_PING_TIME;

    p=imc_remoteinfo;

    for (i=0; i<whichping; i++)
    {
      if (!p)
	break;
      p=p->next;
      if (!p)
      {
	p=imc_remoteinfo;
	whichping=0;
	break;
      }
    }

    if (p)
    {
      whichping++;

      gettimeofday(&tv, NULL);
      imc_send_ping(p->name, tv.tv_sec, tv.tv_usec);
    }
  }

  /* run low-level idle */

  imc_ll_idle();

  /* run mail idle */

  imc_mail_idle();
}

/* shut down all of IMC */
void imc_shutdown(void)
{
  int i;
  imc_reminfo *p, *pnext;

  imc_ll_shutdown();

  for (i=0; i<IMC_RIGNORE_MAX; i++)
    if (imc_rignore[i])
      imc_strfree(imc_rignore[i]);

  for (i=0; i<IMC_MAX; i++)
  {
    if (imc_info[i].inuse)
    {
      imc_strfree(imc_info[i].name);
      imc_strfree(imc_info[i].host);
      imc_strfree(imc_info[i].clientpw);
      imc_strfree(imc_info[i].serverpw);
    }
  }

  for (p=imc_remoteinfo; p; p=pnext)
  {
    pnext=p->next;
    imc_strfree(p->version);
    imc_strfree(p->name);
    imc_free(p, sizeof(imc_reminfo));
  }

  imc_mail_shutdown();

  imc_strfree(imc_prefix);
}

