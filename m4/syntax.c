/* GNU m4 -- A simple macro processor
   Copyright (C) 1989, 1990, 1991, 1992, 1993, 1994, 2002, 2004, 2006,
   2007, 2008 Free Software Foundation, Inc.

   This file is part of GNU M4.

   GNU M4 is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GNU M4 is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>

#include "m4private.h"

/* Define this to see runtime debug info.  Implied by DEBUG.  */
/*#define DEBUG_SYNTAX */

/* THE SYNTAX TABLE

   The input is read character by character and grouped together
   according to a syntax table.  The character groups are (definitions
   are all in m4.h, those marked with a * are not yet in use):

   M4_SYNTAX_IGNORE	*Character to be deleted from input as if not present
   M4_SYNTAX_OTHER	Any character with no special meaning to m4
   M4_SYNTAX_SPACE	Whitespace (ignored when leading macro arguments)
   M4_SYNTAX_OPEN	Open list of macro arguments
   M4_SYNTAX_CLOSE	Close list of macro arguments
   M4_SYNTAX_COMMA	Separates macro arguments
   M4_SYNTAX_DOLLAR	Indicates macro argument in user macros
   M4_SYNTAX_LBRACE	Indicates start of extended macro argument
   M4_SYNTAX_RBRACE	Indicates end of extended macro argument
   M4_SYNTAX_ACTIVE	This character is a macro name by itself
   M4_SYNTAX_ESCAPE	Use this character to prefix all macro names

   M4_SYNTAX_ALPHA	Alphabetic characters (can start macro names)
   M4_SYNTAX_NUM	Numeric characters (can form macro names)

   M4_SYNTAX_LQUOTE	A single characters left quote
   M4_SYNTAX_BCOMM	A single characters begin comment delimiter

   (These are bit masks)
   M4_SYNTAX_RQUOTE	A single characters right quote
   M4_SYNTAX_ECOMM	A single characters end comment delimiter

   Besides adding new facilities, the use of a syntax table will reduce
   the number of calls to next_token ().  Now groups of OTHER, NUM and
   SPACE characters can be returned as a single token, since next_token
   () knows they have no special syntactical meaning to m4.  This is,
   however, only possible if only single character quotes comments
   comments are used, because otherwise the quote and comment characters
   will not show up in the syntax-table.

   Having a syntax table allows new facilities.  The new builtin
   "changesyntax" allows the the user to change the category of any
   character.

   Default '\n' is both ECOMM and SPACE, depending on the context.  To
   solve the problem of quotes and comments that have diffent syntax
   code based on the context, the RQUOTE and ECOMM codes are bit
   masks to add to an ordinary code.  If a character is made a quote it
   will be recognised if the basis code does not have precedence.

   When changing quotes and comment delimiters only the bits are
   removed, and the characters are therefore reverted to its old
   category code.

   The precedence as implemented by next_token () is:

   M4_SYNTAX_IGNORE	*Filtered out below next_token ()
   M4_SYNTAX_ESCAPE	Reads macro name iff set, else next character
   M4_SYNTAX_ALPHA	Reads M4_SYNTAX_ALPHA and M4_SYNTAX_NUM as macro name
   M4_SYNTAX_LQUOTE	Reads all until balanced M4_SYNTAX_RQUOTE
   M4_SYNTAX_BCOMM	Reads all until M4_SYNTAX_ECOMM

   M4_SYNTAX_OTHER  }	Reads all M4_SYNTAX_OTHER, M4_SYNTAX_NUM
   M4_SYNTAX_NUM    }	M4_SYNTAX_DOLLAR, M4_SYNTAX_LBRACE, M4_SYNTAX_RBRACE
   M4_SYNTAX_DOLLAR }
   M4_SYNTAX_LBRACE }
   M4_SYNTAX_RBRACE }

   M4_SYNTAX_SPACE	Reads all M4_SYNTAX_SPACE, depending on buffering
   M4_SYNTAX_ACTIVE	Returns a single char as a macro name

   M4_SYNTAX_OPEN   }	Returned as a single char
   M4_SYNTAX_CLOSE  }
   M4_SYNTAX_COMMA  }

   The $, {, and } are not really a part of m4's input syntax, because a
   a string is parsed equally whether there is a $ or not.  These characters
   are instead used during user macro expansion.

   M4_SYNTAX_RQUOTE and M4_SYNTAX_ECOMM do not start tokens.  */

static bool check_is_single_quotes	(m4_syntax_table *);
static bool check_is_single_comments	(m4_syntax_table *);
static bool check_is_macro_escaped	(m4_syntax_table *);
static int add_syntax_attribute		(m4_syntax_table *, int, int);
static int remove_syntax_attribute	(m4_syntax_table *, int, int);
static void set_quote_age		(m4_syntax_table *, bool, bool);

m4_syntax_table *
m4_syntax_create (void)
{
  m4_syntax_table *syntax = xzalloc (sizeof *syntax);
  int ch;

  /* Set up default table.  This table never changes during operation.  */
  for (ch = UCHAR_MAX + 1; --ch >= 0; )
    switch (ch)
      {
      case '(':
	syntax->orig[ch] = M4_SYNTAX_OPEN;
	break;
      case ')':
	syntax->orig[ch] = M4_SYNTAX_CLOSE;
	break;
      case ',':
	syntax->orig[ch] = M4_SYNTAX_COMMA;
	break;
      case '$':
	syntax->orig[ch] = M4_SYNTAX_DOLLAR;
	break;
      case '{':
	syntax->orig[ch] = M4_SYNTAX_LBRACE;
	break;
      case '}':
	syntax->orig[ch] = M4_SYNTAX_RBRACE;
	break;
      case '`':
	syntax->orig[ch] = M4_SYNTAX_LQUOTE;
	break;
      case '#':
	syntax->orig[ch] = M4_SYNTAX_BCOMM;
	break;
      case '\0':
	/* FIXME - revisit the ignore syntax attribute.  */
	/* syntax->orig[ch] = M4_SYNTAX_IGNORE; */
	/* break; */
      default:
	if (isspace (ch))
	  syntax->orig[ch] = M4_SYNTAX_SPACE;
	else if (isalpha (ch) || ch == '_')
	  syntax->orig[ch] = M4_SYNTAX_ALPHA;
	else if (isdigit (ch))
	  syntax->orig[ch] = M4_SYNTAX_NUM;
	else
	  syntax->orig[ch] = M4_SYNTAX_OTHER;
      }

  /* Set up current table to match default.  */
  m4_set_syntax (syntax, '\0', '\0', NULL);
  syntax->cached_simple.str1 = syntax->cached_lquote;
  syntax->cached_simple.len1 = 1;
  syntax->cached_simple.str2 = syntax->cached_rquote;
  syntax->cached_simple.len2 = 1;
  return syntax;
}

void
m4_syntax_delete (m4_syntax_table *syntax)
{
  assert (syntax);

  free (syntax->quote.str1);
  free (syntax->quote.str2);
  free (syntax->comm.str1);
  free (syntax->comm.str2);
  free (syntax);
}

int
m4_syntax_code (char ch)
{
  int code;

  switch (ch)
    {
       /* Sorted according to the order of M4_SYNTAX_* in m4module.h.  */
       /* FIXME - revisit the ignore syntax attribute.  */
    case 'I': case 'i':	code = M4_SYNTAX_IGNORE; break;
    case '@':		code = M4_SYNTAX_ESCAPE; break;
    case 'W': case 'w':	code = M4_SYNTAX_ALPHA;  break;
    case 'L': case 'l':	code = M4_SYNTAX_LQUOTE; break;
    case 'B': case 'b':	code = M4_SYNTAX_BCOMM;  break;
    case 'O': case 'o':	code = M4_SYNTAX_OTHER;  break;
    case 'D': case 'd':	code = M4_SYNTAX_NUM;    break;
    case '$':		code = M4_SYNTAX_DOLLAR; break;
    case '{':		code = M4_SYNTAX_LBRACE; break;
    case '}':		code = M4_SYNTAX_RBRACE; break;
    case 'S': case 's':	code = M4_SYNTAX_SPACE;  break;
    case 'A': case 'a':	code = M4_SYNTAX_ACTIVE; break;
    case '(':		code = M4_SYNTAX_OPEN;   break;
    case ')':		code = M4_SYNTAX_CLOSE;  break;
    case ',':		code = M4_SYNTAX_COMMA;  break;

    case 'R': case 'r':	code = M4_SYNTAX_RQUOTE; break;
    case 'E': case 'e':	code = M4_SYNTAX_ECOMM;  break;

    default: code = -1;  break;
    }

  return code;
}



/* Functions to manipulate the syntax table.  */
static int
add_syntax_attribute (m4_syntax_table *syntax, int ch, int code)
{
  if (code & M4_SYNTAX_MASKS)
    syntax->table[ch] |= code;
  else
    syntax->table[ch] = (syntax->table[ch] & M4_SYNTAX_MASKS) | code;

#ifdef DEBUG_SYNTAX
  xfprintf(stderr, "Set syntax %o %c = %04X\n",
	   ch, isprint(ch) ? ch : '-',
	   syntax->table[ch]);
#endif

  return syntax->table[ch];
}

static int
remove_syntax_attribute (m4_syntax_table *syntax, int ch, int code)
{
  assert (code & M4_SYNTAX_MASKS);
  syntax->table[ch] &= ~code;

#ifdef DEBUG_SYNTAX
  xfprintf(stderr, "Unset syntax %o %c = %04X\n",
	   ch, isprint(ch) ? ch : '-',
	   syntax->table[ch]);
#endif

  return syntax->table[ch];
}

static void
add_syntax_set (m4_syntax_table *syntax, const char *chars, int code)
{
  int ch;

  if (*chars == '\0')
    return;

  if (code == M4_SYNTAX_ESCAPE)
    syntax->is_macro_escaped = true;

  /* Adding doesn't affect single-quote or single-comment.  */

  while ((ch = to_uchar (*chars++)))
    add_syntax_attribute (syntax, ch, code);
}

static void
subtract_syntax_set (m4_syntax_table *syntax, const char *chars, int code)
{
  int ch;

  if (*chars == '\0')
    return;

  while ((ch = to_uchar (*chars++)))
    {
      if ((code & M4_SYNTAX_MASKS) != 0)
	remove_syntax_attribute (syntax, ch, code);
      else if (m4_has_syntax (syntax, ch, code))
	add_syntax_attribute (syntax, ch, M4_SYNTAX_OTHER);
    }

  /* Check for any cleanup needed.  */
  switch (code)
    {
    case M4_SYNTAX_ESCAPE:
      if (syntax->is_macro_escaped)
	check_is_macro_escaped (syntax);
      break;

    case M4_SYNTAX_LQUOTE:
    case M4_SYNTAX_RQUOTE:
      if (syntax->is_single_quotes)
	check_is_single_quotes (syntax);
      break;

    case M4_SYNTAX_BCOMM:
    case M4_SYNTAX_ECOMM:
      if (syntax->is_single_comments)
	check_is_single_comments (syntax);
      break;

    default:
      break;
    }
}

static void
set_syntax_set (m4_syntax_table *syntax, const char *chars, int code)
{
  int ch;
  /* Explicit set of characters to install with this category; all
     other characters that used to have the category get reset to
     OTHER.  */
  for (ch = UCHAR_MAX + 1; --ch >= 0; )
    {
      if (code == M4_SYNTAX_RQUOTE || code == M4_SYNTAX_ECOMM)
	remove_syntax_attribute (syntax, ch, code);
      else if (m4_has_syntax (syntax, ch, code))
	add_syntax_attribute (syntax, ch, M4_SYNTAX_OTHER);
    }
  while ((ch = to_uchar (*chars++)))
    add_syntax_attribute (syntax, ch, code);

  /* Check for any cleanup needed.  */
  check_is_macro_escaped (syntax);
  check_is_single_quotes (syntax);
  check_is_single_comments (syntax);
}

static void
reset_syntax_set (m4_syntax_table *syntax, int code)
{
  int ch;
  for (ch = UCHAR_MAX + 1; --ch >= 0; )
    {
      /* Reset the category back to its default state.  All other
	 characters that used to have this category get reset to
	 their default state as well.  */
      if (code == M4_SYNTAX_RQUOTE)
	{
	  if (ch == '\'')
	    add_syntax_attribute (syntax, ch, code);
	  else
	    remove_syntax_attribute (syntax, ch, code);
	}
      else if (code == M4_SYNTAX_ECOMM)
	{
	  if (ch == '\n')
	    add_syntax_attribute (syntax, ch, code);
	  else
	    remove_syntax_attribute (syntax, ch, code);
	}
      else if (syntax->orig[ch] == code || m4_has_syntax (syntax, ch, code))
	add_syntax_attribute (syntax, ch, syntax->orig[ch]);
    }
  check_is_macro_escaped (syntax);
  check_is_single_quotes (syntax);
  check_is_single_comments (syntax);
}

int
m4_set_syntax (m4_syntax_table *syntax, char key, char action,
	       const char *chars)
{
  int code;

  assert (syntax);
  assert (chars || key == '\0');

  if (key == '\0')
    {
      /* Restore the default syntax, which has known quote and comment
	 properties.  */
      memcpy (syntax->table, syntax->orig, sizeof syntax->orig);

      free (syntax->quote.str1);
      free (syntax->quote.str2);
      free (syntax->comm.str1);
      free (syntax->comm.str2);

      syntax->quote.str1	= xstrdup (DEF_LQUOTE);
      syntax->quote.len1	= 1;
      syntax->quote.str2	= xstrdup (DEF_RQUOTE);
      syntax->quote.len2	= 1;
      syntax->comm.str1		= xstrdup (DEF_BCOMM);
      syntax->comm.len1		= 1;
      syntax->comm.str2		= xstrdup (DEF_ECOMM);
      syntax->comm.len2		= 1;

      add_syntax_attribute (syntax, to_uchar (syntax->quote.str2[0]),
			    M4_SYNTAX_RQUOTE);
      add_syntax_attribute (syntax, to_uchar (syntax->comm.str2[0]),
			    M4_SYNTAX_ECOMM);

      syntax->is_single_quotes		= true;
      syntax->is_single_comments	= true;
      syntax->is_macro_escaped		= false;
      set_quote_age (syntax, true, false);
      return 0;
    }

  code = m4_syntax_code (key);
  if (code < 0)
    {
      return -1;
    }
  switch (action)
    {
    case '+':
      add_syntax_set (syntax, chars, code);
      break;
    case '-':
      subtract_syntax_set (syntax, chars, code);
      break;
    case '=':
      set_syntax_set (syntax, chars, code);
      break;
    case '\0':
      reset_syntax_set (syntax, code);
      break;
    default:
      assert (false);
    }
  set_quote_age (syntax, false, true);
  m4__quote_uncache (syntax);
  return code;
}

static bool
check_is_single_quotes (m4_syntax_table *syntax)
{
  int ch;
  int lquote = -1;
  int rquote = -1;

  if (! syntax->is_single_quotes)
    return false;
  assert (syntax->quote.len1 == 1 && syntax->quote.len2 == 1);

  if (m4_has_syntax (syntax, *syntax->quote.str1, M4_SYNTAX_LQUOTE)
      && m4_has_syntax (syntax, *syntax->quote.str2, M4_SYNTAX_RQUOTE))
    return true;

  /* The most recent action invalidated our current lquote/rquote.  If
     we still have exactly one character performing those roles based
     on the syntax table, then update lquote/rquote accordingly.
     Otherwise, keep lquote/rquote, but we no longer have single
     quotes.  */
  for (ch = UCHAR_MAX + 1; --ch >= 0; )
    {
      if (m4_has_syntax (syntax, ch, M4_SYNTAX_LQUOTE))
	{
	  if (lquote == -1)
	    lquote = ch;
	  else
	    {
	      syntax->is_single_quotes = false;
	      break;
	    }
	}
      if (m4_has_syntax (syntax, ch, M4_SYNTAX_RQUOTE))
	{
	  if (rquote == -1)
	    rquote = ch;
	  else
	    {
	      syntax->is_single_quotes = false;
	      break;
	    }
	}
    }
  if (lquote == -1 || rquote == -1)
    syntax->is_single_quotes = false;
  else if (syntax->is_single_quotes)
    {
      *syntax->quote.str1 = lquote;
      *syntax->quote.str2 = rquote;
    }
  return syntax->is_single_quotes;
}

static bool
check_is_single_comments (m4_syntax_table *syntax)
{
  int ch;
  int bcomm = -1;
  int ecomm = -1;

  if (! syntax->is_single_comments)
    return false;
  assert (syntax->comm.len1 == 1 && syntax->comm.len2 == 1);

  if (m4_has_syntax (syntax, *syntax->comm.str1, M4_SYNTAX_BCOMM)
      && m4_has_syntax (syntax, *syntax->comm.str2, M4_SYNTAX_ECOMM))
    return true;

  /* The most recent action invalidated our current bcomm/ecomm.  If
     we still have exactly one character performing those roles based
     on the syntax table, then update bcomm/ecomm accordingly.
     Otherwise, keep bcomm/ecomm, but we no longer have single
     comments.  */
  for (ch = UCHAR_MAX + 1; --ch >= 0; )
    {
      if (m4_has_syntax (syntax, ch, M4_SYNTAX_BCOMM))
	{
	  if (bcomm == -1)
	    bcomm = ch;
	  else
	    {
	      syntax->is_single_comments = false;
	      break;
	    }
	}
      if (m4_has_syntax (syntax, ch, M4_SYNTAX_ECOMM))
	{
	  if (ecomm == -1)
	    ecomm = ch;
	  else
	    {
	      syntax->is_single_comments = false;
	      break;
	    }
	}
    }
  if (bcomm == -1 || ecomm == -1)
    syntax->is_single_comments = false;
  else if (syntax->is_single_comments)
    {
      *syntax->comm.str1 = bcomm;
      *syntax->comm.str2 = ecomm;
    }
  return syntax->is_single_comments;
}

static bool
check_is_macro_escaped (m4_syntax_table *syntax)
{
  int ch;

  syntax->is_macro_escaped = false;
  for (ch = UCHAR_MAX + 1; --ch >= 0; )
    if (m4_has_syntax (syntax, ch, M4_SYNTAX_ESCAPE))
      {
	syntax->is_macro_escaped = true;
	break;
      }

  return syntax->is_macro_escaped;
}



/* Functions for setting quotes and comment delimiters.  Used by
   m4_changecom () and m4_changequote ().  Both functions override the
   syntax table to maintain compatibility.  */
void
m4_set_quotes (m4_syntax_table *syntax, const char *lq, const char *rq)
{
  int ch;

  assert (syntax);

  /* POSIX states that with 0 arguments, the default quotes are used.
     POSIX XCU ERN 112 states that behavior is implementation-defined
     if there was only one argument, or if there is an empty string in
     either position when there are two arguments.  We allow an empty
     left quote to disable quoting, but a non-empty left quote will
     always create a non-empty right quote.  See the texinfo for what
     some other implementations do.  */
  if (!lq)
    {
      lq = DEF_LQUOTE;
      rq = DEF_RQUOTE;
    }
  else if (!rq || (*lq && !*rq))
    rq = DEF_RQUOTE;

  if (strcmp (syntax->quote.str1, lq) == 0
      && strcmp (syntax->quote.str2, rq) == 0)
    return;

  free (syntax->quote.str1);
  free (syntax->quote.str2);
  syntax->quote.str1 = xstrdup (lq);
  syntax->quote.len1 = strlen (lq);
  syntax->quote.str2 = xstrdup (rq);
  syntax->quote.len2 = strlen (rq);

  /* changequote overrides syntax_table, but be careful when it is
     used to select a start-quote sequence that is effectively
     disabled.  */

  syntax->is_single_quotes
    = (syntax->quote.len1 == 1 && syntax->quote.len2 == 1
       && !m4_has_syntax (syntax, *syntax->quote.str1,
			  (M4_SYNTAX_IGNORE | M4_SYNTAX_ESCAPE
			   | M4_SYNTAX_ALPHA | M4_SYNTAX_NUM)));

  for (ch = UCHAR_MAX + 1; --ch >= 0; )
    {
      if (m4_has_syntax (syntax, ch, M4_SYNTAX_LQUOTE))
	add_syntax_attribute (syntax, ch,
			      (syntax->orig[ch] == M4_SYNTAX_LQUOTE
			       ? M4_SYNTAX_OTHER : syntax->orig[ch]));
      if (m4_has_syntax (syntax, ch, M4_SYNTAX_RQUOTE))
	remove_syntax_attribute (syntax, ch, M4_SYNTAX_RQUOTE);
    }

  if (syntax->is_single_quotes)
    {
      add_syntax_attribute (syntax, to_uchar (syntax->quote.str1[0]),
			    M4_SYNTAX_LQUOTE);
      add_syntax_attribute (syntax, to_uchar (syntax->quote.str2[0]),
			    M4_SYNTAX_RQUOTE);
    }
  if (syntax->is_macro_escaped)
    check_is_macro_escaped (syntax);
  set_quote_age (syntax, false, false);
}

void
m4_set_comment (m4_syntax_table *syntax, const char *bc, const char *ec)
{
  int ch;

  assert (syntax);

  /* POSIX requires no arguments to disable comments, and that one
     argument use newline as the close-comment.  POSIX XCU ERN 131
     states that empty arguments invoke implementation-defined
     behavior.  We allow an empty begin comment to disable comments,
     and a non-empty begin comment will always create a non-empty end
     comment.  See the texinfo for what some other implementations
     do.  */
  if (!bc)
    bc = ec = "";
  else if (!ec || (*bc && !*ec))
    ec = DEF_ECOMM;

  if (strcmp (syntax->comm.str1, bc) == 0
      && strcmp (syntax->comm.str2, ec) == 0)
    return;

  free (syntax->comm.str1);
  free (syntax->comm.str2);
  syntax->comm.str1 = xstrdup (bc);
  syntax->comm.len1 = strlen (bc);
  syntax->comm.str2 = xstrdup (ec);
  syntax->comm.len2 = strlen (ec);

  /* changecom overrides syntax_table, but be careful when it is used
     to select a start-comment sequence that is effectively
     disabled.  */

  syntax->is_single_comments
    = (syntax->comm.len1 == 1 && syntax->comm.len2 == 1
       && !m4_has_syntax (syntax, *syntax->comm.str1,
			  (M4_SYNTAX_IGNORE | M4_SYNTAX_ESCAPE
			   | M4_SYNTAX_ALPHA | M4_SYNTAX_NUM
			   | M4_SYNTAX_LQUOTE)));

  for (ch = UCHAR_MAX + 1; --ch >= 0; )
    {
      if (m4_has_syntax (syntax, ch, M4_SYNTAX_BCOMM))
	add_syntax_attribute (syntax, ch,
			      (syntax->orig[ch] == M4_SYNTAX_BCOMM
			       ? M4_SYNTAX_OTHER : syntax->orig[ch]));
      if (m4_has_syntax (syntax, ch, M4_SYNTAX_ECOMM))
	remove_syntax_attribute (syntax, ch, M4_SYNTAX_ECOMM);
    }
  if (syntax->is_single_comments)
    {
      add_syntax_attribute (syntax, to_uchar (syntax->comm.str1[0]),
			    M4_SYNTAX_BCOMM);
      add_syntax_attribute (syntax, to_uchar (syntax->comm.str2[0]),
			    M4_SYNTAX_ECOMM);
    }
  if (syntax->is_macro_escaped)
    check_is_macro_escaped (syntax);
  set_quote_age (syntax, false, false);
}

/* Call this when changing anything that might impact the quote age,
   so that m4_quote_age and m4_safe_quotes will reflect the change.
   If RESET, changesyntax was reset to its default stage; if CHANGE,
   arbitrary syntax has changed; otherwise, just quotes or comment
   delimiters have changed.  */
static void
set_quote_age (m4_syntax_table *syntax, bool reset, bool change)
{
  /* Multi-character quotes are inherently unsafe, since concatenation
     of individual characters can result in a quote delimiter,
     consider:

     define(echo,``$1'')define(a,A)changequote(<[,]>)echo(<[]]><[>a]>)
     => A]> (not ]>a)

   Also, unquoted close delimiters are unsafe, consider:

     define(echo,``$1'')define(a,A)echo(`a''`a')
     => aA' (not a'a)

   Duplicated start and end quote delimiters, as well as comment
   delimiters that overlap with quote delimiters or active characters,
   also present a problem, consider:

     define(echo,$*)echo(a,a,a`'define(a,A)changecom(`,',`,'))
     => A,a,A (not A,A,A)

   The impact of arbitrary changesyntax is difficult to characterize.
   So if things are in their default state, we use 0 for the upper 16
   bits of quote_age; otherwise we increment syntax_age for each
   changesyntax, but saturate it at 0xffff rather than wrapping
   around.  Perhaps a cache of other frequently used states is
   warranted, if changesyntax becomes more popular.

   Perhaps someday we will fix $@ expansion to use the current
   settings of the comma category, or even allow multi-character
   argument separators via changesyntax.  Until then, we use a literal
   `,' in $@ expansion, therefore we must insist that `,' be an
   argument separator for quote_age to be non-zero.

   Rather than check every token for an unquoted delimiter, we merely
   encode current_quote_age to 0 when things are unsafe, and non-zero
   when safe (namely, the syntax_age in the upper 16 bits, coupled
   with the 16-bit value composed of the single-character start and
   end quote delimiters).  There may be other situations which are
   safe even when this algorithm sets the quote_age to zero, but at
   least a quote_age of zero always produces correct results (although
   it may take more time in doing so).  */

  unsigned short local_syntax_age;
  if (reset)
    local_syntax_age = 0;
  else if (change && syntax->syntax_age < 0xffff)
    local_syntax_age = ++syntax->syntax_age;
  else
    local_syntax_age = syntax->syntax_age;
  if (local_syntax_age < 0xffff && syntax->is_single_quotes
      && !m4_has_syntax (syntax, *syntax->quote.str1,
			 (M4_SYNTAX_ALPHA | M4_SYNTAX_NUM | M4_SYNTAX_OPEN
			  | M4_SYNTAX_COMMA | M4_SYNTAX_CLOSE
			  | M4_SYNTAX_SPACE))
      && !m4_has_syntax (syntax, *syntax->quote.str2,
			 (M4_SYNTAX_ALPHA | M4_SYNTAX_NUM | M4_SYNTAX_OPEN
			  | M4_SYNTAX_COMMA | M4_SYNTAX_CLOSE
			  | M4_SYNTAX_SPACE))
      && *syntax->quote.str1 != *syntax->quote.str2
      && (!syntax->comm.len1
	  || (*syntax->comm.str1 != *syntax->quote.str2
	      && !m4_has_syntax (syntax, *syntax->comm.str1,
				 (M4_SYNTAX_OPEN | M4_SYNTAX_COMMA
				  | M4_SYNTAX_CLOSE))))
      && m4_has_syntax (syntax, ',', M4_SYNTAX_COMMA))
    {
      syntax->quote_age = ((local_syntax_age << 16)
			   | ((*syntax->quote.str1 & 0xff) << 8)
			   | (*syntax->quote.str2 & 0xff));
    }
  else
    syntax->quote_age = 0;
}

/* Interface for caching frequently used quote pairs, independently of
   the current quote delimiters (for example, consider a text macro
   expansion that includes several copies of $@), and using AGE for
   optimization.  If QUOTES is NULL, don't use quoting.  If OBS is
   non-NULL, AGE should be the current quote age, and QUOTES should be
   m4_get_syntax_quotes; the return value will be a cached quote pair,
   where the pointer is valid at least as long as OBS is not reset,
   but whose contents are only guaranteed until the next changequote
   or quote_cache.  Otherwise, OBS is NULL, AGE should be the same as
   before, and QUOTES should be a previously returned cache value;
   used to refresh the contents of the result.  */
const m4_string_pair *
m4__quote_cache (m4_syntax_table *syntax, m4_obstack *obs, unsigned int age,
		 const m4_string_pair *quotes)
{
  /* Implementation - if AGE is non-zero, then the implementation of
     set_quote_age guarantees that we can recreate the return value on
     the fly; so we use static storage, and the contents must be used
     immediately.  If AGE is zero, then we must copy QUOTES onto OBS,
     but we might as well cache that copy.  */
  if (!quotes)
    return NULL;
  if (age)
    {
      *syntax->cached_lquote = (age >> 8) & 0xff;
      *syntax->cached_rquote = age & 0xff;
      return &syntax->cached_simple;
    }
  if (!obs)
    return quotes;
  assert (quotes == &syntax->quote);
  if (!syntax->cached_quote)
    {
      assert (obstack_object_size (obs) == 0);
      syntax->cached_quote = (m4_string_pair *) obstack_copy (obs, quotes,
							      sizeof *quotes);
      syntax->cached_quote->str1 = (char *) obstack_copy0 (obs, quotes->str1,
							   quotes->len1);
      syntax->cached_quote->str2 = (char *) obstack_copy0 (obs, quotes->str2,
							   quotes->len2);
    }
  return syntax->cached_quote;
}


/* Define these functions at the end, so that calls in the file use the
   faster macro version from m4module.h.  */
#undef m4_get_syntax_lquote
const char *
m4_get_syntax_lquote (m4_syntax_table *syntax)
{
  assert (syntax);
  return syntax->quote.str1;
}

#undef m4_get_syntax_rquote
const char *
m4_get_syntax_rquote (m4_syntax_table *syntax)
{
  assert (syntax);
  return syntax->quote.str2;
}

#undef m4_get_syntax_quotes
const m4_string_pair *
m4_get_syntax_quotes (m4_syntax_table *syntax)
{
  assert (syntax);
  return &syntax->quote;
}

#undef m4_is_syntax_single_quotes
bool
m4_is_syntax_single_quotes (m4_syntax_table *syntax)
{
  assert (syntax);
  return syntax->is_single_quotes;
}

#undef m4_get_syntax_bcomm
const char *
m4_get_syntax_bcomm (m4_syntax_table *syntax)
{
  assert (syntax);
  return syntax->comm.str1;
}

#undef m4_get_syntax_ecomm
const char *
m4_get_syntax_ecomm (m4_syntax_table *syntax)
{
  assert (syntax);
  return syntax->comm.str2;
}

#undef m4_get_syntax_comments
const m4_string_pair *
m4_get_syntax_comments (m4_syntax_table *syntax)
{
  assert (syntax);
  return &syntax->comm;
}

#undef m4_is_syntax_single_comments
bool
m4_is_syntax_single_comments (m4_syntax_table *syntax)
{
  assert (syntax);
  return syntax->is_single_comments;
}

#undef m4_is_syntax_macro_escaped
bool
m4_is_syntax_macro_escaped (m4_syntax_table *syntax)
{
  assert (syntax);
  return syntax->is_macro_escaped;
}
