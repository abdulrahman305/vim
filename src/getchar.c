/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * getchar.c: Code related to getting a character from the user or a script
 * file, manipulations with redo buffer and stuff buffer.
 */

#include "vim.h"

/*
 * These buffers are used for storing:
 * - stuffed characters: A command that is translated into another command.
 * - redo characters: will redo the last change.
 * - recorded characters: for the "q" command.
 *
 * The bytes are stored like in the typeahead buffer:
 * - K_SPECIAL introduces a special key (two more bytes follow).  A literal
 *   K_SPECIAL is stored as K_SPECIAL KS_SPECIAL KE_FILLER.
 * - CSI introduces a GUI termcap code (also when gui.in_use is FALSE,
 *   otherwise switching the GUI on would make mappings invalid).
 *   A literal CSI is stored as CSI KS_EXTRA KE_CSI.
 * These translations are also done on multi-byte characters!
 *
 * Escaping CSI bytes is done by the system-specific input functions, called
 * by ui_inchar().
 * Escaping K_SPECIAL is done by inchar().
 * Un-escaping is done by vgetc().
 */

#define MINIMAL_SIZE 20			// minimal size for b_str

static buffheader_T redobuff = {{NULL, {NUL}}, NULL, 0, 0};
static buffheader_T old_redobuff = {{NULL, {NUL}}, NULL, 0, 0};
static buffheader_T recordbuff = {{NULL, {NUL}}, NULL, 0, 0};

static int typeahead_char = 0;		// typeahead char that's not flushed

#ifdef FEAT_EVAL
static char_u typedchars[MAXMAPLEN + 1] = { NUL };  // typed chars before map
static int typedchars_pos = 0;
#endif

/*
 * When block_redo is TRUE the redo buffer will not be changed.
 * Used by edit() to repeat insertions.
 */
static int	block_redo = FALSE;

static int	KeyNoremap = 0;	    // remapping flags

/*
 * Variables used by vgetorpeek() and flush_buffers().
 *
 * typebuf.tb_buf[] contains all characters that are not consumed yet.
 * typebuf.tb_buf[typebuf.tb_off] is the first valid character.
 * typebuf.tb_buf[typebuf.tb_off + typebuf.tb_len - 1] is the last valid char.
 * typebuf.tb_buf[typebuf.tb_off + typebuf.tb_len] must be NUL.
 * The head of the buffer may contain the result of mappings, abbreviations
 * and @a commands.  The length of this part is typebuf.tb_maplen.
 * typebuf.tb_silent is the part where <silent> applies.
 * After the head are characters that come from the terminal.
 * typebuf.tb_no_abbr_cnt is the number of characters in typebuf.tb_buf that
 * should not be considered for abbreviations.
 * Some parts of typebuf.tb_buf may not be mapped. These parts are remembered
 * in typebuf.tb_noremap[], which is the same length as typebuf.tb_buf and
 * contains RM_NONE for the characters that are not to be remapped.
 * typebuf.tb_noremap[typebuf.tb_off] is the first valid flag.
 * (typebuf has been put in globals.h, because check_termcode() needs it).
 */
#define RM_YES		0	// tb_noremap: remap
#define RM_NONE		1	// tb_noremap: don't remap
#define RM_SCRIPT	2	// tb_noremap: remap local script mappings
#define RM_ABBR		4	// tb_noremap: don't remap, do abbrev.

// typebuf.tb_buf has three parts: room in front (for result of mappings), the
// middle for typeahead and room for new characters (which needs to be 3 *
// MAXMAPLEN for the Amiga).
#define TYPELEN_INIT	(5 * (MAXMAPLEN + 3))
static char_u	typebuf_init[TYPELEN_INIT];	// initial typebuf.tb_buf
static char_u	noremapbuf_init[TYPELEN_INIT];	// initial typebuf.tb_noremap

static size_t	last_recorded_len = 0;	// number of last recorded chars

#ifdef FEAT_EVAL
mapblock_T	*last_used_map = NULL;
int		last_used_sid = -1;
#endif

static int	read_readbuf(buffheader_T *buf, int advance);
static void	init_typebuf(void);
static void	may_sync_undo(void);
static void	free_typebuf(void);
static void	closescript(void);
static void	updatescript(int c);
static int	vgetorpeek(int);
static int	inchar(char_u *buf, int maxlen, long wait_time);
#ifdef FEAT_EVAL
static int	do_key_input_pre(int c);
#endif

/*
 * Free and clear a buffer.
 */
    static void
free_buff(buffheader_T *buf)
{
    buffblock_T	*p, *np;

    for (p = buf->bh_first.b_next; p != NULL; p = np)
    {
	np = p->b_next;
	vim_free(p);
    }
    buf->bh_first.b_next = NULL;
    buf->bh_curr = NULL;
}

/*
 * Return the contents of a buffer as a single string.
 * K_SPECIAL and CSI in the returned string are escaped.
 */
    static char_u *
get_buffcont(
    buffheader_T	*buffer,
    int			dozero)	    // count == zero is not an error
{
    long_u	    count = 0;
    char_u	    *p = NULL;
    char_u	    *p2;
    char_u	    *str;
    buffblock_T *bp;

    // compute the total length of the string
    for (bp = buffer->bh_first.b_next; bp != NULL; bp = bp->b_next)
	count += (long_u)STRLEN(bp->b_str);

    if ((count > 0 || dozero) && (p = alloc(count + 1)) != NULL)
    {
	p2 = p;
	for (bp = buffer->bh_first.b_next; bp != NULL; bp = bp->b_next)
	    for (str = bp->b_str; *str; )
		*p2++ = *str++;
	*p2 = NUL;
    }
    return p;
}

/*
 * Return the contents of the record buffer as a single string
 * and clear the record buffer.
 * K_SPECIAL and CSI in the returned string are escaped.
 */
    char_u *
get_recorded(void)
{
    char_u	*p;
    size_t	len;

    p = get_buffcont(&recordbuff, TRUE);
    free_buff(&recordbuff);

    /*
     * Remove the characters that were added the last time, these must be the
     * (possibly mapped) characters that stopped the recording.
     */
    len = STRLEN(p);
    if (len >= last_recorded_len)
    {
	len -= last_recorded_len;
	p[len] = NUL;
    }

    /*
     * When stopping recording from Insert mode with CTRL-O q, also remove the
     * CTRL-O.
     */
    if (len > 0 && restart_edit != 0 && p[len - 1] == Ctrl_O)
	p[len - 1] = NUL;

    return (p);
}

/*
 * Return the contents of the redo buffer as a single string.
 * K_SPECIAL and CSI in the returned string are escaped.
 */
    char_u *
get_inserted(void)
{
    return get_buffcont(&redobuff, FALSE);
}

/*
 * Add string "s" after the current block of buffer "buf".
 * K_SPECIAL and CSI should have been escaped already.
 */
    static void
add_buff(
    buffheader_T	*buf,
    char_u		*s,
    long		slen)	// length of "s" or -1
{
    buffblock_T *p;
    long_u	    len;

    if (slen < 0)
	slen = (long)STRLEN(s);
    if (slen == 0)				// don't add empty strings
	return;

    if (buf->bh_first.b_next == NULL)	// first add to list
    {
	buf->bh_space = 0;
	buf->bh_curr = &(buf->bh_first);
    }
    else if (buf->bh_curr == NULL)	// buffer has already been read
    {
	iemsg(e_add_to_internal_buffer_that_was_already_read_from);
	return;
    }
    else if (buf->bh_index != 0)
	mch_memmove(buf->bh_first.b_next->b_str,
		    buf->bh_first.b_next->b_str + buf->bh_index,
		    STRLEN(buf->bh_first.b_next->b_str + buf->bh_index) + 1);
    buf->bh_index = 0;

    if (buf->bh_space >= (int)slen)
    {
	len = (long_u)STRLEN(buf->bh_curr->b_str);
	vim_strncpy(buf->bh_curr->b_str + len, s, (size_t)slen);
	buf->bh_space -= slen;
    }
    else
    {
	if (slen < MINIMAL_SIZE)
	    len = MINIMAL_SIZE;
	else
	    len = slen;
	p = alloc(offsetof(buffblock_T, b_str) + len + 1);
	if (p == NULL)
	    return; // no space, just forget it
	buf->bh_space = (int)(len - slen);
	vim_strncpy(p->b_str, s, (size_t)slen);

	p->b_next = buf->bh_curr->b_next;
	buf->bh_curr->b_next = p;
	buf->bh_curr = p;
    }
}

/*
 * Delete "slen" bytes from the end of "buf".
 * Only works when it was just added.
 */
    static void
delete_buff_tail(buffheader_T *buf, int slen)
{
    int len;

    if (buf->bh_curr == NULL)
	return;  // nothing to delete
    len = (int)STRLEN(buf->bh_curr->b_str);
    if (len < slen)
	return;

    buf->bh_curr->b_str[len - slen] = NUL;
    buf->bh_space += slen;
}

/*
 * Add number "n" to buffer "buf".
 */
    static void
add_num_buff(buffheader_T *buf, long n)
{
    char_u	number[32];

    sprintf((char *)number, "%ld", n);
    add_buff(buf, number, -1L);
}

/*
 * Add character 'c' to buffer "buf".
 * Translates special keys, NUL, CSI, K_SPECIAL and multibyte characters.
 */
    static void
add_char_buff(buffheader_T *buf, int c)
{
    char_u	bytes[MB_MAXBYTES + 1];
    int		len;
    int		i;
    char_u	temp[4];

    if (IS_SPECIAL(c))
	len = 1;
    else
	len = (*mb_char2bytes)(c, bytes);
    for (i = 0; i < len; ++i)
    {
	if (!IS_SPECIAL(c))
	    c = bytes[i];

	if (IS_SPECIAL(c) || c == K_SPECIAL || c == NUL)
	{
	    // translate special key code into three byte sequence
	    temp[0] = K_SPECIAL;
	    temp[1] = K_SECOND(c);
	    temp[2] = K_THIRD(c);
	    temp[3] = NUL;
	}
#ifdef FEAT_GUI
	else if (c == CSI)
	{
	    // Translate a CSI to a CSI - KS_EXTRA - KE_CSI sequence
	    temp[0] = CSI;
	    temp[1] = KS_EXTRA;
	    temp[2] = (int)KE_CSI;
	    temp[3] = NUL;
	}
#endif
	else
	{
	    temp[0] = c;
	    temp[1] = NUL;
	}
	add_buff(buf, temp, -1L);
    }
}

// First read ahead buffer. Used for translated commands.
static buffheader_T readbuf1 = {{NULL, {NUL}}, NULL, 0, 0};

// Second read ahead buffer. Used for redo.
static buffheader_T readbuf2 = {{NULL, {NUL}}, NULL, 0, 0};

/*
 * Get one byte from the read buffers.  Use readbuf1 one first, use readbuf2
 * if that one is empty.
 * If advance == TRUE go to the next char.
 * No translation is done K_SPECIAL and CSI are escaped.
 */
    static int
read_readbuffers(int advance)
{
    int c;

    c = read_readbuf(&readbuf1, advance);
    if (c == NUL)
	c = read_readbuf(&readbuf2, advance);
    return c;
}

    static int
read_readbuf(buffheader_T *buf, int advance)
{
    char_u	c;
    buffblock_T	*curr;

    if (buf->bh_first.b_next == NULL)  // buffer is empty
	return NUL;

    curr = buf->bh_first.b_next;
    c = curr->b_str[buf->bh_index];

    if (advance)
    {
	if (curr->b_str[++buf->bh_index] == NUL)
	{
	    buf->bh_first.b_next = curr->b_next;
	    vim_free(curr);
	    buf->bh_index = 0;
	}
    }
    return c;
}

/*
 * Prepare the read buffers for reading (if they contain something).
 */
    static void
start_stuff(void)
{
    if (readbuf1.bh_first.b_next != NULL)
    {
	readbuf1.bh_curr = &(readbuf1.bh_first);
	readbuf1.bh_space = 0;
    }
    if (readbuf2.bh_first.b_next != NULL)
    {
	readbuf2.bh_curr = &(readbuf2.bh_first);
	readbuf2.bh_space = 0;
    }
}

/*
 * Return TRUE if the stuff buffer is empty.
 */
    int
stuff_empty(void)
{
    return (readbuf1.bh_first.b_next == NULL
	 && readbuf2.bh_first.b_next == NULL);
}

#if defined(FEAT_EVAL) || defined(PROTO)
/*
 * Return TRUE if readbuf1 is empty.  There may still be redo characters in
 * redbuf2.
 */
    int
readbuf1_empty(void)
{
    return (readbuf1.bh_first.b_next == NULL);
}
#endif

/*
 * Set a typeahead character that won't be flushed.
 */
    void
typeahead_noflush(int c)
{
    typeahead_char = c;
}

/*
 * Remove the contents of the stuff buffer and the mapped characters in the
 * typeahead buffer (used in case of an error).  If "flush_typeahead" is true,
 * flush all typeahead characters (used when interrupted by a CTRL-C).
 */
    void
flush_buffers(flush_buffers_T flush_typeahead)
{
    init_typebuf();

    start_stuff();
    while (read_readbuffers(TRUE) != NUL)
	;

    if (flush_typeahead == FLUSH_MINIMAL)
    {
	// remove mapped characters at the start only,
	// but only when enough space left in typebuf
	if (typebuf.tb_off + typebuf.tb_maplen >= typebuf.tb_buflen)
	{
	    typebuf.tb_off = MAXMAPLEN;
	    typebuf.tb_len = 0;
	}
	else
	{
	    typebuf.tb_off += typebuf.tb_maplen;
	    typebuf.tb_len -= typebuf.tb_maplen;
	}
#if defined(FEAT_CLIENTSERVER) || defined(FEAT_EVAL)
	if (typebuf.tb_len == 0)
	    typebuf_was_filled = FALSE;
#endif
    }
    else
    {
	// remove typeahead
	if (flush_typeahead == FLUSH_INPUT)
	    // We have to get all characters, because we may delete the first
	    // part of an escape sequence.  In an xterm we get one char at a
	    // time and we have to get them all.
	    while (inchar(typebuf.tb_buf, typebuf.tb_buflen - 1, 10L) != 0)
		;
	typebuf.tb_off = MAXMAPLEN;
	typebuf.tb_len = 0;
#if defined(FEAT_CLIENTSERVER) || defined(FEAT_EVAL)
	// Reset the flag that text received from a client or from feedkeys()
	// was inserted in the typeahead buffer.
	typebuf_was_filled = FALSE;
#endif
    }
    typebuf.tb_maplen = 0;
    typebuf.tb_silent = 0;
    cmd_silent = FALSE;
    typebuf.tb_no_abbr_cnt = 0;
    if (++typebuf.tb_change_cnt == 0)
	typebuf.tb_change_cnt = 1;
}

/*
 * The previous contents of the redo buffer is kept in old_redobuffer.
 * This is used for the CTRL-O <.> command in insert mode.
 */
    void
ResetRedobuff(void)
{
    if (block_redo)
	return;

    free_buff(&old_redobuff);
    old_redobuff = redobuff;
    redobuff.bh_first.b_next = NULL;
}

/*
 * Discard the contents of the redo buffer and restore the previous redo
 * buffer.
 */
    void
CancelRedo(void)
{
    if (block_redo)
	return;

    free_buff(&redobuff);
    redobuff = old_redobuff;
    old_redobuff.bh_first.b_next = NULL;
    start_stuff();
    while (read_readbuffers(TRUE) != NUL)
	;
}

/*
 * Save redobuff and old_redobuff to save_redobuff and save_old_redobuff.
 * Used before executing autocommands and user functions.
 */
    void
saveRedobuff(save_redo_T *save_redo)
{
    char_u	*s;

    save_redo->sr_redobuff = redobuff;
    redobuff.bh_first.b_next = NULL;
    save_redo->sr_old_redobuff = old_redobuff;
    old_redobuff.bh_first.b_next = NULL;

    // Make a copy, so that ":normal ." in a function works.
    s = get_buffcont(&save_redo->sr_redobuff, FALSE);
    if (s == NULL)
	return;

    add_buff(&redobuff, s, -1L);
    vim_free(s);
}

/*
 * Restore redobuff and old_redobuff from save_redobuff and save_old_redobuff.
 * Used after executing autocommands and user functions.
 */
    void
restoreRedobuff(save_redo_T *save_redo)
{
    free_buff(&redobuff);
    redobuff = save_redo->sr_redobuff;
    free_buff(&old_redobuff);
    old_redobuff = save_redo->sr_old_redobuff;
}

/*
 * Append "s" to the redo buffer.
 * K_SPECIAL and CSI should already have been escaped.
 */
    void
AppendToRedobuff(char_u *s)
{
    if (!block_redo)
	add_buff(&redobuff, s, -1L);
}

/*
 * Append to Redo buffer literally, escaping special characters with CTRL-V.
 * K_SPECIAL and CSI are escaped as well.
 */
    void
AppendToRedobuffLit(
    char_u	*str,
    int		len)	    // length of "str" or -1 for up to the NUL
{
    char_u	*s = str;
    int		c;
    char_u	*start;

    if (block_redo)
	return;

    while (len < 0 ? *s != NUL : s - str < len)
    {
	// Put a string of normal characters in the redo buffer (that's
	// faster).
	start = s;
	while (*s >= ' ' && *s < DEL && (len < 0 || s - str < len))
	    ++s;

	// Don't put '0' or '^' as last character, just in case a CTRL-D is
	// typed next.
	if (*s == NUL && (s[-1] == '0' || s[-1] == '^'))
	    --s;
	if (s > start)
	    add_buff(&redobuff, start, (long)(s - start));

	if (*s == NUL || (len >= 0 && s - str >= len))
	    break;

	// Handle a special or multibyte character.
	if (has_mbyte)
	    // Handle composing chars separately.
	    c = mb_cptr2char_adv(&s);
	else
	    c = *s++;
	if (c < ' ' || c == DEL || (*s == NUL && (c == '0' || c == '^')))
	    add_char_buff(&redobuff, Ctrl_V);

	// CTRL-V '0' must be inserted as CTRL-V 048
	if (*s == NUL && c == '0')
	    add_buff(&redobuff, (char_u *)"048", 3L);
	else
	    add_char_buff(&redobuff, c);
    }
}

/*
 * Append "s" to the redo buffer, leaving 3-byte special key codes unmodified
 * and escaping other K_SPECIAL and CSI bytes.
 */
    void
AppendToRedobuffSpec(char_u *s)
{
    if (block_redo)
	return;

    while (*s != NUL)
    {
	if (*s == K_SPECIAL && s[1] != NUL && s[2] != NUL)
	{
	    // Insert special key literally.
	    add_buff(&redobuff, s, 3L);
	    s += 3;
	}
	else
	    add_char_buff(&redobuff, mb_cptr2char_adv(&s));
    }
}

/*
 * Append a character to the redo buffer.
 * Translates special keys, NUL, CSI, K_SPECIAL and multibyte characters.
 */
    void
AppendCharToRedobuff(int c)
{
    if (!block_redo)
	add_char_buff(&redobuff, c);
}

/*
 * Append a number to the redo buffer.
 */
    void
AppendNumberToRedobuff(long n)
{
    if (!block_redo)
	add_num_buff(&redobuff, n);
}

/*
 * Append string "s" to the stuff buffer.
 * CSI and K_SPECIAL must already have been escaped.
 */
    void
stuffReadbuff(char_