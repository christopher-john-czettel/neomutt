/*
 * Copyright (C) 1996-2002 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2000 Thomas Roessler <roessler@guug.de>
 * 
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 */ 

#include "mutt.h"
#include "mutt_menu.h"
#include "rfc1524.h"
#include "mime.h"
#include "mailbox.h"
#include "mapping.h"
#ifdef USE_IMAP
#include "mx.h"
#include "imap.h"
#endif

#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

static struct mapping_t PostponeHelp[] = {
  { N_("Exit"),  OP_EXIT },
  { N_("Del"),   OP_DELETE },
  { N_("Undel"), OP_UNDELETE },
  { N_("Help"),  OP_HELP },
  { NULL }
};



#ifdef HAVE_PGP
#include "pgp.h"
#endif /* HAVE_PGP */

#ifdef HAVE_SMIME
#include "smime.h"
#endif /* HAVE_SMIME */


static short PostCount = 0;
static CONTEXT *PostContext = NULL;
static short UpdateNumPostponed = 0;

/* Return the number of postponed messages.
 * if force is 0, use a cached value if it is costly to get a fresh
 * count (IMAP) - else check.
 */
int mutt_num_postponed (int force)
{
  struct stat st;
  CONTEXT ctx;

  static time_t LastModify = 0;
  static char *OldPostponed = NULL;

  if (UpdateNumPostponed)
  {
    UpdateNumPostponed = 0;
    force = 1;
  }

  if (Postponed != OldPostponed)
  {
    OldPostponed = Postponed;
    LastModify = 0;
    force = 1;
  }

  if (!Postponed)
    return 0;

#ifdef USE_IMAP
  /* LastModify is useless for IMAP */
  if (mx_is_imap (Postponed))
  {
    if (force)
    {
      short newpc;

      newpc = imap_mailbox_check (Postponed, 0);
      if (newpc >= 0)
      {
	PostCount = newpc;
	dprint (2, (debugfile, "mutt_num_postponed: %d postponed IMAP messages found.\n", PostCount));
      }
      else
	dprint (2, (debugfile, "mutt_num_postponed: using old IMAP postponed count.\n"));
    }
    return PostCount;
  }
#endif

  if (stat (Postponed, &st) == -1)
  {
     PostCount = 0;
     LastModify = 0;
     return (0);
  }

  if (S_ISDIR (st.st_mode))
  {
    /* if we have a maildir mailbox, we need to stat the "new" dir */

    char buf[_POSIX_PATH_MAX];

    snprintf (buf, sizeof (buf), "%s/new", Postponed);
    if (access (buf, F_OK) == 0 && stat (buf, &st) == -1)
    {
      PostCount = 0;
      LastModify = 0;
      return 0;
    }
  }

  if (LastModify < st.st_mtime)
  {
    LastModify = st.st_mtime;

    if (access (Postponed, R_OK | F_OK) != 0)
      return (PostCount = 0);
    if (mx_open_mailbox (Postponed, M_NOSORT | M_QUIET, &ctx) == NULL)
      PostCount = 0;
    else
      PostCount = ctx.msgcount;
    mx_fastclose_mailbox (&ctx);
  }

  return (PostCount);
}

void mutt_update_num_postponed (void)
{
  UpdateNumPostponed = 1;
}

static void post_entry (char *s, size_t slen, MUTTMENU *menu, int entry)
{
  CONTEXT *ctx = (CONTEXT *) menu->data;

  _mutt_make_string (s, slen, NONULL (HdrFmt), ctx, ctx->hdrs[entry],
		     M_FORMAT_ARROWCURSOR);
}

static HEADER *select_msg (void)
{
  MUTTMENU *menu;
  int i, done=0, r=-1;
  char helpstr[SHORT_STRING];

  menu = mutt_new_menu ();
  menu->make_entry = post_entry;
  menu->menu = MENU_POST;
  menu->max = PostContext->msgcount;
  menu->title = _("Postponed Messages");
  menu->data = PostContext;
  menu->help = mutt_compile_help (helpstr, sizeof (helpstr), MENU_POST, PostponeHelp);

  while (!done)
  {
    switch (i = mutt_menuLoop (menu))
    {
      case OP_DELETE:
      case OP_UNDELETE:
	mutt_set_flag (PostContext, PostContext->hdrs[menu->current], M_DELETE, (i == OP_DELETE) ? 1 : 0);
	PostCount = PostContext->msgcount - PostContext->deleted;
	if (option (OPTRESOLVE) && menu->current < menu->max - 1)
	{
	  menu->oldcurrent = menu->current;
	  menu->current++;
	  if (menu->current >= menu->top + menu->pagelen)
	  {
	    menu->top = menu->current;
	    menu->redraw = REDRAW_INDEX | REDRAW_STATUS;
	  }
	  else
	    menu->redraw |= REDRAW_MOTION_RESYNCH;
	}
	else
	  menu->redraw = REDRAW_CURRENT;
	break;

      case OP_GENERIC_SELECT_ENTRY:
	r = menu->current;
	done = 1;
	break;

      case OP_EXIT:
	done = 1;
	break;
    }
  }

  mutt_menuDestroy (&menu);
  return (r > -1 ? PostContext->hdrs[r] : NULL);
}

/* args:
 *      ctx	Context info, used when recalling a message to which
 *              we reply.
 *	hdr	envelope/attachment info for recalled message
 *	cur	if message was a reply, `cur' is set to the message which
 *		`hdr' is in reply to
 *	fcc	fcc for the recalled message
 *	fcclen	max length of fcc
 *
 * return vals:
 *	-1		error/no messages
 *	0		normal exit
 *	SENDREPLY	recalled message is a reply
 */
int mutt_get_postponed (CONTEXT *ctx, HEADER *hdr, HEADER **cur, char *fcc, size_t fcclen)
{
  HEADER *h;
  int code = SENDPOSTPONED;
  LIST *tmp;
  LIST *last = NULL;
  LIST *next;
  char *p;
  int opt_delete;

  if (!Postponed)
    return (-1);

  if ((PostContext = mx_open_mailbox (Postponed, M_NOSORT, NULL)) == NULL)
  {
    PostCount = 0;
    mutt_error _("No postponed messages.");
    return (-1);
  }
  
  if (! PostContext->msgcount)
  {
    PostCount = 0;
    mx_close_mailbox (PostContext, NULL);
    safe_free ((void **) &PostContext);
    mutt_error _("No postponed messages.");
    return (-1);
  }

  if (PostContext->msgcount == 1)
  {
    /* only one message, so just use that one. */
    h = PostContext->hdrs[0];
  }
  else if ((h = select_msg ()) == NULL)
  {
    mx_close_mailbox (PostContext, NULL);
    safe_free ((void **) &PostContext);
    return (-1);
  }

  if (mutt_prepare_template (NULL, PostContext, hdr, h, 0) < 0)
  {
    mx_fastclose_mailbox (PostContext);
    safe_free ((void **) &PostContext);
    return (-1);
  }

  /* finished with this message, so delete it. */
  mutt_set_flag (PostContext, h, M_DELETE, 1);

  /* update the count for the status display */
  PostCount = PostContext->msgcount - PostContext->deleted;

  /* avoid the "purge deleted messages" prompt */
  opt_delete = quadoption (OPT_DELETE);
  set_quadoption (OPT_DELETE, M_YES);
  mx_close_mailbox (PostContext, NULL);
  set_quadoption (OPT_DELETE, opt_delete);

  safe_free ((void **) &PostContext);

  for (tmp = hdr->env->userhdrs; tmp; )
  {
    if (ascii_strncasecmp ("X-Mutt-References:", tmp->data, 18) == 0)
    {
      if (ctx)
      {
	/* if a mailbox is currently open, look to see if the orignal message
	   the user attempted to reply to is in this mailbox */
	p = tmp->data + 18;
	SKIPWS (p);
	if (!ctx->id_hash)
	  ctx->id_hash = mutt_make_id_hash (ctx);
	*cur = hash_find (ctx->id_hash, p);
      }

      /* Remove the X-Mutt-References: header field. */
      next = tmp->next;
      if (last)
	last->next = tmp->next;
      else
	hdr->env->userhdrs = tmp->next;
      tmp->next = NULL;
      mutt_free_list (&tmp);
      tmp = next;
      if (*cur)
	code |= SENDREPLY;
    }
    else if (ascii_strncasecmp ("X-Mutt-Fcc:", tmp->data, 11) == 0)
    {
      p = tmp->data + 11;
      SKIPWS (p);
      strfcpy (fcc, p, fcclen);
      mutt_pretty_mailbox (fcc);

      /* remove the X-Mutt-Fcc: header field */
      next = tmp->next;
      if (last)
	last->next = tmp->next;
      else
	hdr->env->userhdrs = tmp->next;
      tmp->next = NULL;
      mutt_free_list (&tmp);
      tmp = next;
    }



#ifdef HAVE_PGP
    else if (mutt_strncmp ("Pgp:", tmp->data, 4) == 0 /* this is generated
						       * by old mutt versions
						       */
	     || mutt_strncmp ("X-Mutt-PGP:", tmp->data, 11) == 0)
    {
      hdr->security = mutt_parse_crypt_hdr (strchr (tmp->data, ':') + 1, 1);
      hdr->security |= APPLICATION_PGP;
       
      /* remove the pgp field */
      next = tmp->next;
      if (last)
	last->next = tmp->next;
      else
	hdr->env->userhdrs = tmp->next;
      tmp->next = NULL;
      mutt_free_list (&tmp);
      tmp = next;
    }
#endif /* HAVE_PGP */


#ifdef HAVE_SMIME
    else if (mutt_strncmp ("X-Mutt-SMIME:", tmp->data, 13) == 0)
    {
      hdr->security = mutt_parse_crypt_hdr (strchr (tmp->data, ':') + 1, 1);
      hdr->security |= APPLICATION_SMIME;
       
      /* remove the smime field */
      next = tmp->next;
      if (last)
	last->next = tmp->next;
      else
	hdr->env->userhdrs = tmp->next;
      tmp->next = NULL;
      mutt_free_list (&tmp);
      tmp = next;
    }
#endif /* HAVE_SMIME */


#ifdef MIXMASTER
    else if (mutt_strncmp ("X-Mutt-Mix:", tmp->data, 11) == 0)
    {
      char *t;
      mutt_free_list (&hdr->chain);
      
      t = strtok (tmp->data + 11, " \t\n");
      while (t)
      {
	hdr->chain = mutt_add_list (hdr->chain, t);
	t = strtok (NULL, " \t\n");
      }
      
      next = tmp->next;
      if (last) 
	last->next = tmp->next;
      else
	hdr->env->userhdrs = tmp->next;
      tmp->next = NULL;
      mutt_free_list (&tmp);
      tmp = next;
    }
#endif

    else
    {
      last = tmp;
      tmp = tmp->next;
    }
  }
  return (code);
}



#if defined(HAVE_PGP) || defined(HAVE_SMIME)

int mutt_parse_crypt_hdr (char *p, int set_signas)
{
  int pgp = 0;
  char pgp_sign_as[LONG_STRING] = "\0", *q;
  char smime_cryptalg[LONG_STRING] = "\0";
   
  SKIPWS (p);
  for (; *p; p++)
  {    
     
    switch (*p)
    {
      case 'e':
      case 'E':
        pgp |= ENCRYPT;
        break;

      case 's':    
      case 'S':
        pgp |= SIGN;
        q = pgp_sign_as;
      
        if (*(p+1) == '<')
        {
          for (p += 2; 
	       *p && *p != '>' && q < pgp_sign_as + sizeof (pgp_sign_as) - 1;
               *q++ = *p++)
	    ;

          if (*p!='>')
          {
            mutt_error _("Illegal PGP header");
            return 0;
          }
        }
       
        *q = '\0';
        break;

      /* This used to be the micalg parameter.
       * 
       * It's no longer needed, so we just skip the parameter in order
       * to be able to recall old messages.
       */
      case 'm':
      case 'M':
        if(*(p+1) == '<')
        {
	  for (p += 2; *p && *p != '>'; p++)
	    ;
	  if(*p != '>')
	  {
	    mutt_error _("Illegal PGP header");
	    return 0;
	  }
	}

	break;
	  
	  
      case 'c':
      case 'C':
   	q = smime_cryptalg;
	
        if(*(p+1) == '<')
	{
	  for(p += 2; *p && *p != '>' && q < smime_cryptalg + sizeof(smime_cryptalg) - 1;
	      *q++ = *p++)
	    ;
	  
	  if(*p != '>')
	  {
	    mutt_error _("Illegal S/MIME header");
	    return 0;
	  }
	}

	*q = '\0';
	break;

      default:
        mutt_error _("Illegal PGP header");
        return 0;
    }
     
  }
 
  /* the cryptalg field must not be empty */
#ifdef HAVE_SMIME
  if (*smime_cryptalg)
    mutt_str_replace (&SmimeCryptAlg, smime_cryptalg);
#endif /*  HAVE_SMIME */

#ifdef HAVE_PGP
  if (set_signas || *pgp_sign_as)
    mutt_str_replace (&PgpSignAs, pgp_sign_as);
#endif /* HAVE_PGP */

  return pgp;
}
#endif /* HAVE_PGP ||  HAVE_SMIME */



int mutt_prepare_template (FILE *fp, CONTEXT *ctx, HEADER *newhdr, HEADER *hdr,
			       short weed)
{
  MESSAGE *msg = NULL;
  char file[_POSIX_PATH_MAX];
  BODY *b;
  FILE *bfp;
  
  int rv = -1;
  STATE s;
  
  memset (&s, 0, sizeof (s));
  
  if (!fp && (msg = mx_open_message (ctx, hdr->msgno)) == NULL)
    return (-1);

  if (!fp) fp = msg->fp;

  bfp = fp;

  /* parse the message header and MIME structure */

  fseek (fp, hdr->offset, 0);
  newhdr->offset = hdr->offset;
  newhdr->env = mutt_read_rfc822_header (fp, newhdr, 1, weed);
  newhdr->content->length = hdr->content->length;
  mutt_parse_part (fp, newhdr->content);

  safe_free ((void **) &newhdr->env->message_id);
  safe_free ((void **) &newhdr->env->mail_followup_to); /* really? */

#ifdef HAVE_PGP
  /* decrypt pgp/mime encoded messages */
  /* XXX - what happens with S/MIME encrypted messages?!?!?  - tlr, 020909*/
  if ((hdr->security & APPLICATION_PGP) && 
      mutt_is_multipart_encrypted (newhdr->content))
  {
    newhdr->security |= PGPENCRYPT;
    if (!pgp_valid_passphrase())
      goto err;

    mutt_message _("Invoking PGP...");
    if (pgp_decrypt_mime (fp, &bfp, newhdr->content, &b) == -1 || b == NULL)
    {
 err:
      mx_close_message (&msg);
      mutt_free_envelope (&newhdr->env);
      mutt_free_body (&newhdr->content);
      mutt_error _("Decryption failed.");
      return -1;
    }

    mutt_free_body (&newhdr->content);
    newhdr->content = b;

    mutt_clear_error ();
  }
#endif

#if defined(HAVE_PGP)|| defined(HAVE_SMIME)

  /* 
   * remove a potential multipart/signed layer - useful when
   * resending messages 
   */
  
  if (mutt_is_multipart_signed (newhdr->content))
  {
    newhdr->security |= SIGN;
#ifdef HAVE_PGP
    if (ascii_strcasecmp (mutt_get_parameter ("protocol", newhdr->content->parameter),
			  "application/pgp-signature") == 0)
      newhdr->security |= APPLICATION_PGP;
#endif
#if defined (HAVE_PGP) && defined (HAVE_SMIME)
    else
#endif
#ifdef HAVE_SMIME
      newhdr->security |= APPLICATION_SMIME;
#endif
    
    /* destroy the signature */
    mutt_free_body (&newhdr->content->parts->next);
    newhdr->content = mutt_remove_multipart (newhdr->content);
  }
#endif

  /* 
   * We don't need no primary multipart.
   * Note: We _do_ preserve messages!
   * 
   * XXX - we don't handle multipart/alternative in any 
   * smart way when sending messages.  However, one may
   * consider this a feature.
   * 
   */

  if (newhdr->content->type == TYPEMULTIPART)
    newhdr->content = mutt_remove_multipart (newhdr->content);

  s.fpin = bfp;
  
  /* create temporary files for all attachments */
  for (b = newhdr->content; b; b = b->next)
  {
    
    /* what follows is roughly a receive-mode variant of
     * mutt_get_tmp_attachment () from muttlib.c
     */

    file[0] = '\0';
    if (b->filename)
    {
      strfcpy (file, b->filename, sizeof (file));
      b->d_filename = safe_strdup (b->filename);
    }
    else
    {
      /* avoid Content-Disposition: header with temporary filename */
      b->use_disp = 0;
    }

    /* set up state flags */

    s.flags = 0;

    if (b->type == TYPETEXT)
    {
      if (!ascii_strcasecmp ("yes", mutt_get_parameter ("x-mutt-noconv", b->parameter)))
	b->noconv = 1;
      else
      {
	s.flags |= M_CHARCONV;
	b->noconv = 0;
      }

      mutt_delete_parameter ("x-mutt-noconv", &b->parameter);
    }

    mutt_adv_mktemp (file, sizeof(file));
    if ((s.fpout = safe_fopen (file, "w")) == NULL)
      goto bail;

    
    mutt_decode_attachment (b, &s);

    if (safe_fclose (&s.fpout) != 0)
      goto bail;

    mutt_str_replace (&b->filename, file);
    b->unlink = 1;

    mutt_stamp_attachment (b);

    mutt_free_body (&b->parts);
    if (b->hdr) b->hdr->content = NULL; /* avoid dangling pointer */
  }

  rv = 0;
  
  bail:
  
  /* that's it. */
  if (bfp != fp) fclose (bfp);
  if (msg) mx_close_message (&msg);
  
  if (rv == -1)
  {
    mutt_free_envelope (&newhdr->env);
    mutt_free_body (&newhdr->content);
  }
  
  return rv;
}
