/* fetchmail.c
 * Get mail using the pop3 protocol.
 * Format the mail in ascii, or in html.
 * Unpack attachments.
 * Copyright (c) Karl Dahlke, 2006
 * This file is part of the edbrowse project, released under GPL.
 */

#include "eb.h"

#define MHLINE 120		/* length of a mail header line */
#define BAD64 1
#define EXTRA64 2
struct MHINFO {
    struct MHINFO *next, *prev;
    struct listHead components;
    char *start, *end;
    char subject[MHLINE];
    char to[MHLINE];
    char from[MHLINE];
    char reply[MHLINE];
    char date[MHLINE];
    char boundary[MHLINE];
    int boundlen;
    char cfn[MHLINE];		/* content file name */
    uchar ct, ce;		/* content type, content encoding */
    bool andOthers;
    bool doAttach;
    bool atimage;
    uchar error64;
    bool ne;			/* non english */
};

static int nattach;		/* number of attachments */
static int nimages;		/* number of attached images */
static char *firstAttach;	/* name of first file */
static bool mailIsHtml, mailIsSpam, mailIsBlack;
static char *fm;		/* formatted mail string */
static int fm_l;
static struct MHINFO *lastMailInfo;
static char *lastMailText;
#define MAXIPBLACK 3000
static IP32bit ipblacklist[MAXIPBLACK];
static IP32bit ipblackmask[MAXIPBLACK];
static bool ipblackcomp[MAXIPBLACK];
static int nipblack;

void
loadBlacklist(void)
{
    char *buf;
    int buflen;
    char *s, *t;
    if(!ipbFile)
	return;
    if(!fileIntoMemory(ipbFile, &buf, &buflen))
	showErrorAbort();
    s = buf;
    while(*s) {
	t = strchr(s, '\n');
	if(!t)
	    t = s + strlen(s);
	else
	    *t++ = 0;
	while(isspaceByte(*s))
	    ++s;
	if(isdigitByte(*s)) {
	    IP32bit ip;
	    IP32bit ipmask;
	    char dotstop = 0;
	    char *q = strpbrk(s, "/!");
	    if(q)
		dotstop = *q, *q++ = 0;
	    ip = tcp_dots_ip(s);
	    if(ip != -1) {
		ipmask = 0xffffffff;
		if(q) {
		    int bits;
		    if(dotstop == '!')
			bits = 0;
		    else {
			bits = atoi(q);
			q = strchr(q, '!');
			if(q)
			    dotstop = '!';
		    }
		    if(bits > 0 && bits < 32) {
			static const IP32bit masklist[] = {
			    0xffffffff, 0xfeffffff, 0xfcffffff, 0xf8ffffff,
			    0xf0ffffff, 0xe0ffffff, 0xc0ffffff, 0x80ffffff,
			    0x00ffffff, 0x00feffff, 0x00fcffff, 0x00f8ffff,
			    0x00f0ffff, 0x00e0ffff, 0x00c0ffff, 0x0080ffff,
			    0x0000ffff, 0x0000feff, 0x0000fcff, 0x0000f8ff,
			    0x0000f0ff, 0x0000e0ff, 0x0000c0ff, 0x000080ff,
			    0x000000ff, 0x000000fe, 0x000000fc, 0x000000f8,
			    0x000000f0, 0x000000e0, 0x000000c0, 0x00000080,
			    0
			};
			ipmask = masklist[32 - bits];
		    }
		}
		if(ipmask)
		    ip &= ipmask;
		if(nipblack == MAXIPBLACK)
		    i_printfExit(MSG_ManyIP, MAXIPBLACK);
		ipblacklist[nipblack] = ip;
		ipblackmask[nipblack] = ipmask;
		ipblackcomp[nipblack] = (dotstop == '!');
		++nipblack;
	    }
	}
	s = t;
    }
    nzFree(buf);
    debugPrint(3, "%d ip addresses in blacklist", nipblack);
}				/* loadBlacklist */

bool
onBlacklist1(IP32bit tip)
{
    IP32bit blip;		/* black ip */
    IP32bit mask;
    int j;
    for(j = 0; j < nipblack; ++j) {
	bool comp = ipblackcomp[j];
	blip = ipblacklist[j];
	mask = ipblackmask[j];
	if((tip ^ blip) & mask)
	    continue;
	if(comp)
	    return false;
	debugPrint(3, "blocked by rule %d", j + 1);
	return true;
    }
    return false;
}				/* onBlacklist1 */

static bool
onBlacklist(void)
{
    IP32bit *ipp = cw->iplist;
    IP32bit tip;		/* test ip */
    if(!ipp)
	return false;
    while((tip = *ipp++) != NULL_IP)
	if(onBlacklist1(tip))
	    return true;
    return false;
}				/* onBlacklist */

static void
freeMailInfo(struct MHINFO *w)
{
    struct MHINFO *v;
    while(!listIsEmpty(&w->components)) {
	v = w->components.next;
	delFromList(v);
	freeMailInfo(v);
    }
    nzFree(w);
}				/* freeMailInfo */

static char *
getFileName(const char *defname, bool isnew)
{
    static char buf[ABSPATH];
    int l;
    char *p;
    while(true) {
	i_printf(MSG_FileName);
	if(defname)
	    printf("[%s] ", defname);
	if(!fgets(buf, sizeof (buf), stdin))
	    exit(0);
	for(p = buf; isspaceByte(*p); ++p) ;
	l = strlen(p);
	while(l && isspaceByte(p[l - 1]))
	    --l;
	p[l] = 0;
	if(!l) {
	    if(!defname)
		continue;
/* make a copy just to be safe */
	    strcpy(buf, defname);
	    p = buf;
	} else
	    defname = 0;
	if(isnew && fileTypeByName(p, false)) {
	    i_printf(MSG_FileExists, p);
	    defname = 0;
	    continue;
	}
	return p;
    }
}				/* getFileName */

static bool ignoreImages;

static void
writeAttachment(struct MHINFO *w)
{
    const char *atname;
    if((ismc | ignoreImages) && w->atimage)
	return;			/* image ignored */
    if(w->error64 == BAD64)
	i_printf(MSG_Abbreviated);
    if(w->start == w->end) {
	i_printf(MSG_AttEmpty);
	if(w->cfn[0])
	    printf(" %s", w->cfn);
	nl();
	atname = "x";
    } else {
	i_printf(MSG_Att);
	atname = getFileName((w->cfn[0] ? w->cfn : 0), true);
/* X is like x, but deletes all future images */
	if(stringEqual(atname, "X")) {
	    atname = "x";
	    ignoreImages = true;
	}
    }
    if(!ismc && stringEqual(atname, "e")) {
	int cx, svcx = context;
	for(cx = 1; cx < MAXSESSION; ++cx)
	    if(!sessionList[cx].lw)
		break;
	if(cx == MAXSESSION) {
	    i_printf(MSG_AttNoBuffer);
	} else {
	    cxSwitch(cx, false);
	    i_printf(MSG_SessionX, cx);
	    if(!addTextToBuffer((pst) w->start, w->end - w->start, 0))
		i_printf(MSG_AttNoCopy, cx);
	    else if(w->cfn[0])
		cw->fileName = cloneString(w->cfn);
	    cxSwitch(svcx, false);	/* back to where we were */
	}
    } else if(!stringEqual(atname, "x")) {
	int fh = open(atname, O_WRONLY | O_BINARY | O_CREAT | O_TRUNC, 0666);
	if(fh < 0) {
	    i_printf(MSG_AttNoSave, atname);
	    if(ismc)
		exit(1);
	} else {
	    int nb = w->end - w->start;
	    if(write(fh, w->start, nb) < nb) {
		i_printf(MSG_AttNoWrite, atname);
		if(ismc)
		    exit(1);
	    }
	    close(fh);
	}
    }
}				/* writeAttachment */

static void
writeAttachments(struct MHINFO *w)
{
    struct MHINFO *v;
    if(w->doAttach) {
	writeAttachment(w);
    } else {
	foreach(v, w->components)
	   writeAttachments(v);
    }
}				/* writeAttachments */

static void
serverPutGet(const char *line, bool secure)
{
    if(!serverPutLine(line, secure))
	showErrorAbort();
    if(!serverGetLine(secure))
	showErrorAbort();
}				/* serverPutGet */

void
fetchMail(int account)
{
    const struct MACCOUNT *a = accounts + account - 1;
    const char *login = a->login;
    const char *pass = a->password;
    const char *host = a->inurl;
    int nmsgs, m, j, k;

    if(!isInteractive) {
	if(!zapMail)
	    i_printfExit(MSG_FetchNotBackgnd);
    }

    if(!mailDir)
	i_printfExit(MSG_NoMailDir);
    if(chdir(mailDir))
	i_printfExit(MSG_NoDirChange, mailDir);

    if(!loadAddressBook())
	showErrorAbort();

    if(!mailConnect(host, a->inport, a->inssl))
	showErrorAbort();
    if(!serverGetLine(a->inssl))
	showErrorAbort();
    if(memcmp(serverLine, "+OK ", 4))
	i_printfExit(MSG_BadPopIntro, serverLine);
    sprintf(serverLine, "user %s%s", login, eol);
    serverPutGet(serverLine, a->inssl);
    if(pass) {			/* I think this is always required */
	sprintf(serverLine, "pass %s%s", pass, eol);
	serverPutGet(serverLine, a->inssl);
    }				/* password */
    if(memcmp(serverLine, "+OK", 3))
	i_printfExit(MSG_PopNotComplete, serverLine);

/* How many mail messages? */
    serverPutGet("stat\r\n", a->inssl);
    if(memcmp(serverLine, "+OK ", 4))
	i_printfExit(MSG_NoStatusMailBox, serverLine);
    nmsgs = atoi(serverLine + 4);
    if(!nmsgs) {
	i_puts(MSG_NoMail);
	serverClose(a->inssl);
	exit(0);
    }

    if(!(zapMail | passMail)) {
/* We look up a lot of hosts; make a tcp connect */
	sethostent(1);
/* the conect will drop when the program exits */
    }

    i_printf(MSG_MessagesX, nmsgs);
    if(zapMail && nmsgs > 300)
	nmsgs = 300;

    for(m = 1; m <= nmsgs; ++m) {
	const char *redirect = 0;	/* send mail elsewhere */
	char key;
	const char *atname;	/* name of attachment */
	bool delflag = false;	/* delete this mail */
	int exact_l;
	char *exact = initString(&exact_l);	/* the exact message */
	int displine;

	if(zapMail)
	    delflag = true;
	else {			/* read the next message */
	    char retrbuf[5000];
	    bool retr1;
/* We need to clear out the editor, from the last message,
 * then read in this one, in its entirety.
 * This is probably a waste, since the user might delete a megabyte message
 * after seeing only the first paragraph, or even the subject,
 * but as for programming, it's easier to read the whole thing in right now. */
	    if(lastMailInfo)
		freeMailInfo(lastMailInfo);
	    lastMailInfo = 0;
	    nzFree(lastMailText);
	    lastMailText = 0;
	    if(sessionList[1].lw)
		cxQuit(1, 2);
	    cs = 0;
	    linesReset();
	    cxSwitch(1, false);
/* Now grab the entire message */
	    sprintf(serverLine, "retr %d%s", m, eol);
	    if(!serverPutLine(serverLine, a->inssl))
		showErrorAbort();
	    retr1 = true;
	    while(true) {
		int nr;
		if(a->inssl)
		    nr = ssl_read(retrbuf, sizeof (retrbuf));
		else
		    nr = tcp_read(mssock, retrbuf, sizeof (retrbuf));
		if(nr <= 0)
		    i_printfExit(MSG_ErrorReadMess, errno);
		if(retr1) {
/* add null, to make it easy to print the error message, if necessary */
		    if(nr < sizeof (retrbuf))
			retrbuf[nr] = 0;
		    if(memcmp(retrbuf, "+OK", 3))
			i_printfExit(MSG_ErrorFetchMess, m, retrbuf);
		    j = 3;
		    while(retrbuf[j] != '\n')
			++j;
		    ++j;
		    nr -= j;
		    memcpy(retrbuf, retrbuf + j, nr);
		    retr1 = false;
		}
		if(nr)
		    stringAndBytes(&exact, &exact_l, retrbuf, nr);
/* . by itself on a line ends the transmission */
		j = exact_l - 1;
		if(j < 0)
		    continue;
		if(exact[j] != '\n')
		    continue;
		--j;
		if(j >= 0 && exact[j] == '\r')
		    --j;
		if(j < 0)
		    continue;
		if(exact[j] != '.')
		    continue;
		if(!j)
		    break;
		if(exact[j - 1] == '\n')
		    break;
	    }
	    exact_l = j;

/* get rid of the dos returns */
	    for(j = k = 0; j < exact_l; ++j) {
		if(exact[j] == '\r' && j < exact_l - 1 && exact[j + 1] == '\n')
		    continue;
		exact[k++] = exact[j];
	    }
	    exact_l = k;
	    exact[k] = 0;

	    if(!addTextToBuffer((pst) exact, exact_l, 0))
		showErrorAbort();
	    if(!unformatMail) {
		browseCurrentBuffer();
		if(!passMail) {
		    mailIsBlack = onBlacklist();
		    redirect = mailRedirect(lastMailInfo->to,
		       lastMailInfo->from, lastMailInfo->reply,
		       lastMailInfo->subject);
		}
		if(redirect) {
		    key = 'w';
		    if(*redirect == '-')
			++redirect, key = 'u';
		    if(stringEqual(redirect, "x"))
			i_puts(MSG_Junk);
		    else
			printf("> %s\n", redirect);
		} else if((mailIsSpam | mailIsBlack) && spamCan) {
		    redirect = spamCan;
		    key = 'u';
		    i_printf(MSG_Spam);
		    if(lastMailInfo->from[0]) {
			i_printf(MSG_From);
			printf("%s", lastMailInfo->from);
		    } else if(lastMailInfo->reply[0]) {
			i_printf(MSG_From);
			printf("%s", lastMailInfo->reply);
		    }
		    nl();
		} else if(!nattach &&	/* drop empty mail message */
		   cw->dol -
		   (lastMailInfo->subject != 0) -
		   (lastMailInfo->from != 0) -
		   (lastMailInfo->reply != 0) <= 1) {
		    redirect = "x";
		    i_puts(MSG_Empty);
		}
	    }
	    if(redirect)
		delflag = true;

/* display the next page of mail and get a command from the keyboard */
	    displine = 1;
	    while(true) {
		if(!delflag) {	/* show next page */
		  nextpage:
		    if(displine <= cw->dol) {
			for(j = 0; j < 20 && displine <= cw->dol;
			   ++j, ++displine) {
			    char *showline = (char *)fetchLine(displine, 1);
			    k = pstLength((pst) showline);
			    showline[--k] = 0;
			    printf("%s\n", showline);
			    nzFree(showline);
			}
		    }
		}

/* get key command */
		while(true) {
		    if(!delflag) {
/* interactive prompt depends on whether there is more text or not */
			printf("%c ", displine > cw->dol ? '?' : '*');
			fflush(stdout);
			key = getLetter("qx? nwkudijJ");
			printf("\b\b\b");
			fflush(stdout);

			switch (key) {
			case 'q':
			    i_puts(MSG_Quit);
			    serverClose(a->inssl);
			case 'x':
			    exit(0);
			case 'n':
			    i_puts(MSG_Next);
			    goto afterinput;
			case 'd':
			    i_puts(MSG_Delete);
			    delflag = true;
			    goto afterinput;
			case 'i':
			    i_puts(MSG_IPDelete);
			    if(!cw->iplist || cw->iplist[0] == -1) {
				if(passMail)
				    i_puts(MSG_POption);
				else
				    i_puts(ipbFile ? MSG_None :
				       MSG_NoBlackList);
			    } else {
				IP32bit addr;
				for(k = 0; (addr = cw->iplist[k]) != NULL_IP;
				   ++k) {
				    puts(tcp_ip_dots(addr));
				    if(nipblack == MAXIPBLACK)
					continue;
				    ipblacklist[nipblack] = addr;
				    ipblackmask[nipblack] = 0xffffffff;
				    ipblackcomp[nipblack] = false;
				    ++nipblack;
				}
			    }
			    delflag = true;
			    goto afterinput;
			case 'j':
			case 'J':
			    i_puts(MSG_Junk);
			    if(!junkSubject(lastMailInfo->subject, key))
				continue;
			    delflag = true;
			    goto afterinput;
			case ' ':
			    if(displine > cw->dol)
				i_puts(MSG_EndMessage);
			    goto nextpage;
			case '?':
			    i_puts(MSG_MailHelp);
			    continue;
			case 'k':
			case 'w':
			case 'u':
			    break;
			default:
			    i_puts(MSG_NYI);
			    continue;
			}	/* switch */
		    }

		    /* delflag or not */
		    /* At this point we're saving the mail somewhere. */
		    if(key != 'k')
			delflag = true;
		    atname = redirect;
		  savemail:
		    if(!atname)
			atname = getFileName(0, false);
		    if(!stringEqual(atname, "x")) {
			char exists = fileTypeByName(atname, false);
			int fsize;	/* file size */
			int fh =
			   open(atname, O_WRONLY | O_TEXT | O_CREAT | O_APPEND,
			   0666);
			if(fh < 0) {
			    i_printf(MSG_NoCreate, atname);
			    goto savemail;
			}
			if(exists)
			    write(fh,
			       "======================================================================\n",
			       71);
			if(key == 'u' || unformatMail) {
			    if(write(fh, exact, exact_l) < exact_l) {
			      badsave:
				i_printf(MSG_NoWrite, atname);
				close(fh);
				goto savemail;
			    }
			    close(fh);
			    fsize = exact_l;
			} else {
			    fsize = 0;
			    for(j = 1; j <= cw->dol; ++j) {
				char *showline = (char *)fetchLine(j, 1);
				int len = pstLength((pst) showline);
				if(write(fh, showline, len) < len)
				    goto badsave;
				nzFree(showline);
				fsize += len;
			    }	/* loop over lines */
			    close(fh);
			    if(nattach)
				writeAttachments(lastMailInfo);
			}	/* unformat or format */
			if(atname != spamCan) {
			    i_printf(MSG_MailSaved, fsize);
			    if(exists)
				i_printf(MSG_Appended);
			    nl();
			}
		    }		/* saving to a real file */
		    goto afterinput;

		}		/* key commands */
	    }			/* paging through the message */
	}			/* interactive or zap */
      afterinput:

	if(delflag) {
/* Remember, it isn't really gone until you quit the session.
 * So if you didn't mean to delete, type x to exit abruptly,
 * then fetch your mail again. */
	    sprintf(serverLine, "dele %d%s", m, eol);
	    if(!serverPutLine(serverLine, a->inssl))
		showErrorAbort();
	    if(!serverGetLine(a->inssl))
		i_printfExit(MSG_MailTimeOver);
	    if(memcmp(serverLine, "+OK", 3))
		i_printfExit(MSG_UnableDelMail, serverLine);
	}
	/* deleted */
	nzFree(exact);
    }				/* loop over mail messages */

    if(zapMail)
	printf("%d\n", nmsgs);
    serverClose(a->inssl);
    exit(0);
}				/* fetchMail */

/* Here are the common keywords for mail header lines.
 * These are in alphabetical order, so you can stick more in as you find them.
 * The more words we have, the more accurate the test. */
static const char *const mhwords[] = {
    "action:",
    "arrival-date:",
    "bcc:",
    "cc:",
    "content-transfer-encoding:",
    "content-type:",
    "date:",
    "delivered-to:",
    "errors-to:",
    "final-recipient:",
    "from:",
    "importance:",
    "last-attempt-date:",
    "list-id:",
    "mailing-list:",
    "message-id:",
    "mime-version:",
    "precedence:",
    "received:",
    "remote-mta:",
    "reply-to:",
    "reporting-mta:",
    "return-path:",
    "sender:",
    "sent:",
    "status:",
    "subject:",
    "to:",
    "x-beenthere:",
    "x-comment:",
    "x-loop:",
    "x-mailer:",
    "x-mailman-version:",
    "x-mdaemon-deliver-to:",
    "x-mdremoteip:",
    "x-mimeole:",
    "x-ms-tnef-correlator:",
    "x-msmail-priority:",
    "x-originating-ip:",
    "x-priority:",
    "x-return-path:",
    "X-Spam-Checker-Version:",
    "x-spam-level:",
    "x-spam-msg-id:",
    "X-SPAM-Msg-Sniffer-Result:",
    "x-spam-processed:",
    "x-spam-status:",
    "x-uidl:",
    0
};

/* Before we render a mail message, let's make sure it looks like email.
 * This is similar to htmlTest() in html.c. */
bool
emailTest(void)
{
    int i, j, k, n;

/* This is a very simple test - hopefully not too simple.
 * The first 20 non-indented lines have to look like mail header lines,
 * with at least half the keywords recognized. */
    for(i = 1, j = k = 0; i <= cw->dol && j < 20; ++i) {
	char *q;
	char *p = (char *)fetchLine(i, -1);
	char first = *p;
	if(first == '\n' || first == '\r' && p[1] == '\n')
	    break;
	if(first == ' ' || first == '\t')
	    continue;
	++j;			/* nonindented line */
	for(q = p; isalnumByte(*q) || *q == '_' || *q == '-'; ++q) ;
	if(q == p)
	    continue;
	if(*q++ != ':')
	    continue;
/* X-Whatever is a mail header word */
	if(q - p >= 8 && p[1] == '-' && toupper(p[0]) == 'X') {
	    ++k;
	} else {
	    for(n = 0; mhwords[n]; ++n)
		if(memEqualCI(mhwords[n], p, q - p))
		    break;
	    if(mhwords[n])
		++k;
	}
	if(k >= 4 && k * 2 >= j)
	    return true;
    }				/* loop over lines */

    return false;
}				/* emailTest */

static uchar
unb64(char c)
{
    if(isupperByte(c))
	return c - 'A';
    if(islowerByte(c))
	return c - ('a' - 26);
    if(isdigitByte(c))
	return c - ('0' - 52);
    if(c == '+')
	return 62;
    if(c == '/')
	return 63;
    return 64;			/* error */
}				/* unb64 */

static void
unpack64(struct MHINFO *w)
{
    uchar val, leftover, mod;
    bool equals;
    char c, *q, *r;
/* Since this is a copy, and the unpacked version is always
 * smaller, just unpack it inline. */
    mod = 0;
    equals = false;
    for(q = r = w->start; q < w->end; ++q) {
	c = *q;
	if(isspaceByte(c))
	    continue;
	if(equals) {
	    if(c == '=')
		continue;
	    runningError(MSG_AttAfterChars);
	    w->error64 = EXTRA64;
	    break;
	}
	if(c == '=') {
	    equals = true;
	    continue;
	}
	val = unb64(c);
	if(val & 64) {
	    runningError(MSG_AttBad64);
	    w->error64 = BAD64;
	    break;
	}
	if(mod == 0) {
	    leftover = val << 2;
	} else if(mod == 1) {
	    *r++ = (leftover | (val >> 4));
	    leftover = val << 4;
	} else if(mod == 2) {
	    *r++ = (leftover | (val >> 2));
	    leftover = val << 6;
	} else {
	    *r++ = (leftover | val);
	}
	++mod;
	mod &= 3;
    }
    w->end = r;
}				/* unpack64 */

static void
unpackQP(struct MHINFO *w)
{
    uchar val;
    char c, d, *q, *r;
    for(q = r = w->start; q < w->end; ++q) {
	c = *q;
	if(c != '=') {
	    *r++ = c;
	    continue;
	}
	c = *++q;
	if(c == '\n')
	    continue;
	d = q[1];
	if(isxdigit(c) && isxdigit(d)) {
	    d = fromHex(c, d);
	    if(d == 0)
		d = ' ';
	    *r++ = d;
	    ++q;
	    continue;
	}
	--q;
	*r++ = '=';
    }
    w->end = r;
    *r = 0;
}				/* unpackQP */

/* Look for the name of the attachment and boundary */
static void
ctExtras(struct MHINFO *w, const char *s, const char *t)
{
    char quote;
    const char *q, *al, *ar;

    if(w->ct < CT_MULTI) {
	quote = 0;
	for(q = s + 1; q < t; ++q) {
	    if(isalnumByte(q[-1]))
		continue;
/* could be name= or filename= */
	    if(memEqualCI(q, "file", 4))
		q += 4;
	    if(!memEqualCI(q, "name=", 5))
		continue;
	    q += 5;
	    if(*q == '"') {
		quote = *q;
		++q;
	    }
	    for(al = q; q < t; ++q) {
		if(*q == '"')
		    break;
		if(quote)
		    continue;
		if(strchr(",; \t", *q))
		    break;
	    }
	    ar = q;
	    if(ar - al >= MHLINE)
		ar = al + MHLINE - 1;
	    strncpy(w->cfn, al, ar - al);
	    break;
	}
    }
    /* regular file */
    if(w->ct >= CT_MULTI) {
	quote = 0;
	for(q = s + 1; q < t; ++q) {
	    if(isalnumByte(q[-1]))
		continue;
	    if(!memEqualCI(q, "boundary=", 9))
		continue;
	    q += 9;
	    if(*q == '"') {
		quote = *q;
		++q;
	    }
	    for(al = q; q < t; ++q) {
		if(*q == '"')
		    break;
		if(quote)
		    continue;
		if(strchr(",; \t", *q))
		    break;
	    }
	    ar = q;
	    w->boundlen = ar - al;
	    strncpy(w->boundary, al, ar - al);
	    break;
	}
    }				/* multi or alt */
}				/* ctExtras */

/* Now that we know it's mail, see what information we can
 * glean from the headers.
 * Returns a pointer to an allocated MHINFO structure.
 * This routine is recursive. */
static struct MHINFO *
headerGlean(char *start, char *end)
{
    char *s, *t, *q;
    char *vl, *vr;		/* value left and value right */
    struct MHINFO *w;
    int j, k, n;
    char linetype = 0;

/* defaults */
    w = allocZeroMem(sizeof (struct MHINFO));
    initList(&w->components);
    w->ct = CT_OTHER;
    w->ce = CE_8BIT;
    w->andOthers = false;
    w->start = start, w->end = end;

    for(s = start; s < end; s = t + 1) {
	char quote;
	char first = *s;
	t = strchr(s, '\n');
	if(!t)
	    t = end - 1;	/* should never happen */
	if(t == s)
	    break;		/* empty line */

	if(first == ' ' || first == '\t') {
	    if(linetype == 'c')
		ctExtras(w, s, t);
	    continue;
	}

/* find the lead word */
	for(q = s; isalnumByte(*q) || *q == '_' || *q == '-'; ++q) ;
	if(q == s)
	    continue;		/* should never happen */
	if(*q++ != ':')
	    continue;		/* should never happen */
	for(vl = q; *vl == ' ' || *vl == '\t'; ++vl) ;
	for(vr = t; vr > vl && (vr[-1] == ' ' || vr[-1] == '\t'); --vr) ;
	if(vr == vl)
	    continue;		/* empty */
	if(vr - vl > MHLINE - 1)
	    vr = vl + MHLINE - 1;

/* This is sort of a switch statement on the word */
	if(memEqualCI(s, "subject:", q - s)) {
	    linetype = 's';
	    if(w->subject[0])
		continue;
/* get rid of forward/reply prefixes */
	    for(q = vl; q < vr; ++q) {
		static const char *const prefix[] = {
		    "re", "sv", "fwd", 0
		};
		if(!isalphaByte(*q))
		    continue;
		if(q > vl && isalnumByte(q[-1]))
		    continue;
		for(j = 0; prefix[j]; ++j)
		    if(memEqualCI(q, prefix[j], strlen(prefix[j])))
			break;
		if(!prefix[j])
		    continue;
		j = strlen(prefix[j]);
		if(!strchr(":-,;", q[j]))
		    continue;
		++j;
		while(q + j < vr && q[j] == ' ')
		    ++j;
		memcpy(q, q + j, vr - q - j);
		vr -= j;
		--q;		/* try again */
	    }
	    strncpy(w->subject, vl, vr - vl);
/* If the subject is really long, spreads onto the next line,
 * I'll just use ... */
	    if(t < end - 1 && (t[1] == ' ' || t[1] == '\t'))
		strcat(w->subject, "...");
	    continue;
	}
	/* subject */
	if(memEqualCI(s, "reply-to:", q - s)) {
	    linetype = 'r';
	    if(!w->reply[0])
		strncpy(w->reply, vl, vr - vl);
	    continue;
	}
	/* reply */
	if(memEqualCI(s, "from:", q - s)) {
	    linetype = 'f';
	    if(!w->from[0])
		strncpy(w->from, vl, vr - vl);
	    continue;
	}
	/* from */
	if(memEqualCI(s, "date:", q - s) || memEqualCI(s, "sent:", q - s)) {
	    linetype = 'd';
	    if(w->date[0])
		continue;
/* don't need the weekday, seconds, or timezone */
	    if(vr - vl > 5 &&
	       isalphaByte(vl[0]) && isalphaByte(vl[1]) && isalphaByte(vl[2]) &&
	       vl[3] == ',' && vl[4] == ' ')
		vl += 5;
	    strncpy(w->date, vl, vr - vl);
	    q = strrchr(w->date, ':');
	    if(q)
		*q = 0;
	    continue;
	}
	/* date */
	if(memEqualCI(s, "to:", q - s)) {
	    linetype = 't';
	    if(w->to[0])
		continue;
	    strncpy(w->to, vl, vr - vl);
/* Only retain the first recipient */
	    quote = 0;
	    for(q = w->to; *q; ++q) {
		if(*q == ',' && !quote) {
		    w->andOthers = true;
		    break;
		}
		if(*q == '"') {
		    if(!quote)
			quote = *q;
		    else if(quote == *q)
			quote = 0;
		    continue;
		}
		if(*q == '<') {
		    if(!quote)
			quote = *q;
		    continue;
		}
		if(*q == '>') {
		    if(quote == '<')
			quote = 0;
		    continue;
		}
	    }
	    *q = 0;		/* cut it off at the comma */
	    continue;
	}
	/* to */
	if(memEqualCI(s, "cc:", q - s) || memEqualCI(s, "bcc:", q - s)) {
	    w->andOthers = true;
	    continue;
	}
	/* carbon copy */
	if(memEqualCI(s, "content-type:", q - s)) {
	    linetype = 'c';
	    if(memEqualCI(vl, "text", 4))
		w->ct = CT_RICH;
	    if(memEqualCI(vl, "text/html", 9))
		w->ct = CT_HTML;
	    if(memEqualCI(vl, "text/plain", 10))
		w->ct = CT_TEXT;
	    if(memEqualCI(vl, "application", 11))
		w->ct = CT_APPLIC;
	    if(memEqualCI(vl, "multipart", 9))
		w->ct = CT_MULTI;
	    if(memEqualCI(vl, "multipart/alternative", 21))
		w->ct = CT_ALT;

	    ctExtras(w, s, t);
	    continue;
	}
	/* content type */
	if(memEqualCI(s, "content-transfer-encoding:", q - s)) {
	    linetype = 'e';
	    if(memEqualCI(vl, "quoted-printable", 16))
		w->ce = CE_QP;
	    if(memEqualCI(vl, "7bit", 4))
		w->ce = CE_7BIT;
	    if(memEqualCI(vl, "8bit", 4))
		w->ce = CE_8BIT;
	    if(memEqualCI(vl, "base64", 6))
		w->ce = CE_64;
	    continue;
	}
	/* content transfer encoding */
	linetype = 0;
    }				/* loop over lines */

    w->start = start = s + 1;

/* Fix up reply and from lines.
 * From should be the name, reply the address. */
    if(!w->from[0])
	strcpy(w->from, w->reply);
    if(!w->reply[0])
	strcpy(w->reply, w->from);
    if(w->from[0] == '"') {
	strcpy(w->from, w->from + 1);
	q = strchr(w->from, '"');
	if(q)
	    *q = 0;
    }
    vl = strchr(w->from, '<');
    vr = strchr(w->from, '>');
    if(vl && vr && vl < vr) {
	while(vl > w->from && vl[-1] == ' ')
	    --vl;
	*vl = 0;
    }
    vl = strchr(w->reply, '<');
    vr = strchr(w->reply, '>');
    if(vl && vr && vl < vr) {
	*vr = 0;
	strcpy(w->reply, vl + 1);
    }
/* get rid of (name) comment */
    vl = strchr(w->reply, '(');
    vr = strchr(w->reply, ')');
    if(vl && vr && vl < vr) {
	while(vl > w->reply && vl[-1] == ' ')
	    --vl;
	*vl = 0;
    }
/* no @ means it's not an email address */
    if(!strchr(w->reply, '@'))
	w->reply[0] = 0;
    if(stringEqual(w->reply, w->from))
	w->from[0] = 0;
    vl = strchr(w->to, '<');
    vr = strchr(w->to, '>');
    if(vl && vr && vl < vr) {
	*vr = 0;
	strcpy(w->to, vl + 1);
    }

    if(debugLevel >= 5) {
	puts("mail header analyzed");
	printf("subject: %s\n", w->subject);
	printf("from: %s\n", w->from);
	printf("date: %s\n", w->date);
	printf("reply: %s\n", w->reply);
	printf("boundary: %d|%s\n", w->boundlen, w->boundary);
	printf("filename: %s\n", w->cfn);
	printf("content %d/%d\n", w->ct, w->ce);
    }
    /* debug */
    if(w->ce == CE_QP)
	unpackQP(w);
    if(w->ce == CE_64)
	unpack64(w);
    if(w->ce == CE_64 && w->ct == CT_OTHER || w->ct == CT_APPLIC || w->cfn[0]) {
	w->doAttach = true;
	++nattach;
	q = w->cfn;
	if(*q) {		/* name present */
	    if(stringEqual(q, "winmail.dat")) {
		w->atimage = true;
		++nimages;
	    } else if((q = strrchr(q, '.'))) {
		static const char *const imagelist[] = {
		    "gif", "jpg", "tif", "bmp", "asc", "png", 0
		};
/* the asc isn't an image, it's a signature card. */
/* Similarly for the winmail.dat */
		if(stringInListCI(imagelist, q + 1) >= 0) {
		    w->atimage = true;
		    ++nimages;
		}
	    }
	    if(!w->atimage && nattach == nimages + 1)
		firstAttach = w->cfn;
	}
	return w;
    }

/* loop over the mime components */
    if(w->ct == CT_MULTI || w->ct == CT_ALT) {
	char *lastbound = 0;
	bool endmode = false;
	struct MHINFO *child;
/* We really need the -1 here, because sometimes the boundary will
 * be the very first thing in the message body. */
	s = w->start - 1;
	while(!endmode && (t = strstr(s, "\n--")) && t < end) {
	    if(memcmp(t + 3, w->boundary, w->boundlen)) {
		s = t + 3;
		continue;
	    }
	    q = t + 3 + w->boundlen;
	    while(*q == '-')
		endmode = true, ++q;
	    if(*q == '\n')
		++q;
	    debugPrint(5, "boundary found at offset %d", t - w->start);
	    if(lastbound) {
		child = headerGlean(lastbound, t);
		addToListBack(&w->components, child);
	    }
	    s = lastbound = q;
	}
	w->start = w->end = 0;
	return w;
    }

    /* mime or alt */
    /* Scan through, we might have a mail message included inline */
    vl = 0;			/* first mail header keyword line */
    for(s = start; s < end; s = t + 1) {
	char first = *s;
	t = strchr(s, '\n');
	if(!t)
	    t = end - 1;	/* should never happen */
	if(t == s) {		/* empty line */
	    if(!vl)
		continue;
/* Do we have enough for a mail header? */
	    if(k >= 4 && k * 2 >= j) {
		struct MHINFO *child = headerGlean(vl, end);
		addToListBack(&w->components, child);
		w->end = end = vl;
		goto textonly;
	    }			/* found mail message inside */
	    vl = 0;
	}			/* empty line */
	if(first == ' ' || first == '\t')
	    continue;		/* indented */
	for(q = s; isalnumByte(*q) || *q == '_' || *q == '-'; ++q) ;
	if(q == s || *q != ':') {
	    vl = 0;
	    continue;
	}
/* looks like header: stuff */
	if(!vl) {
	    vl = s;
	    j = k = 0;
	}
	++j;
	for(n = 0; mhwords[n]; ++n)
	    if(memEqualCI(mhwords[n], s, q - s))
		break;
	if(mhwords[n])
	    ++k;
    }				/* loop over lines */

/* Header could be at the very end */
    if(vl && k >= 4 && k * 2 >= j) {
	struct MHINFO *child = headerGlean(vl, end);
	addToListBack(&w->components, child);
	w->end = end = vl;
    }

  textonly:
/* Any additional processing of the text, from start to end, can go here. */
/* Remove leading blank lines or lines with useless words */
    for(s = start; s < end; s = t + 1) {
	t = strchr(s, '\n');
	if(!t)
	    t = end - 1;	/* should never happen */
	vl = s, vr = t;
	if(vr - vl >= 4 && memEqualCI(vr - 4, "<br>", 4))
	    vr -= 4;
	while(vl < vr) {
	    if(isalnumByte(*vl))
		break;
	    ++vl;
	}
	while(vl < vr) {
	    if(isalnumByte(vr[-1]))
		break;
	    --vr;
	}
	if(vl == vr)
	    continue;		/* empty */
	if(memEqualCI(vl, "forwarded message", vr - vl))
	    continue;
	if(memEqualCI(vl, "original message", vr - vl))
	    continue;
	break;			/* something real */
    }
    w->start = start = s;

    w->ne = false;

    return w;
}				/* headerGlean */

static char *
headerShow(struct MHINFO *w, bool top)
{
    static char buf[(MHLINE + 30) * 4];
    static char lastsubject[MHLINE];
    char *s;
    bool lines = false;
    buf[0] = 0;

    if(!(w->subject[0] | w->from[0] | w->reply[0]))
	return buf;

    if(!top) {
	strcpy(buf, "Message");
	lines = true;
	if(w->from[0]) {
	    strcat(buf, " from ");
	    strcat(buf, w->from);
	}
	if(w->subject[0]) {
	    if(stringEqual(w->subject, lastsubject)) {
		strcat(buf, " with the same subject");
	    } else {
		strcat(buf, " with subject: ");
		strcat(buf, w->subject);
	    }
	} else
	    strcat(buf, " with no subject");
	if(mailIsHtml) {	/* trash & < > */
	    for(s = buf; *s; ++s) {
/* This is quick and stupid */
		if(*s == '<')
		    *s = '(';
		if(*s == '>')
		    *s = ')';
		if(*s == '&')
		    *s = '*';
	    }
	}
/* need a dot at the end? */
	s = buf + strlen(buf);
	if(isalnumByte(s[-1]))
	    *s++ = '.';
	strcpy(s, mailIsHtml ? "\n<br>" : "\n");
	if(w->date[0]) {
	    strcat(buf, "Sent ");
	    strcat(buf, w->date);
	}
	if(w->reply) {
	    if(!w->date[0])
		strcat(buf, "From ");
	    else
		strcat(buf, " from ");
	    strcat(buf, w->reply);
	}
	if(w->date[0] | w->reply[0]) {	/* second line */
	    strcat(buf, "\n");
	}
    } else {

/* This is at the top of the file */
	if(w->subject[0]) {
	    sprintf(buf, "Subject: %s\n", w->subject);
	    lines = true;
	}
	if(nattach && ismc) {
	    char atbuf[20];
	    if(lines & mailIsHtml)
		strcat(buf, "<br>");
	    lines = true;
	    if(nimages) {
		sprintf(atbuf, "%d images\n", nimages);
		if(nimages == 1)
		    strcpy(atbuf, "1 image");
		strcat(buf, atbuf);
		if(nattach > nimages)
		    strcat(buf, " + ");
	    }
	    if(nattach == nimages + 1) {
		strcat(buf, "1 attachment");
		if(firstAttach && firstAttach[0]) {
		    strcat(buf, " ");
		    strcat(buf, firstAttach);
		}
	    }
	    if(nattach > nimages + 1) {
		sprintf(atbuf, "%d attachments\n", nattach - nimages);
		strcat(buf, atbuf);
	    }
	    strcat(buf, "\n");
	}
	/* attachments */
	if(w->to[0] && !ismc) {
	    if(lines & mailIsHtml)
		strcat(buf, "<br>");
	    lines = true;
	    strcat(buf, "To ");
	    strcat(buf, w->to);
	    if(w->andOthers)
		strcat(buf, " and others");
	    strcat(buf, "\n");
	}
	if(w->from[0]) {
	    if(lines & mailIsHtml)
		strcat(buf, "<br>");
	    lines = true;
	    strcat(buf, "From ");
	    strcat(buf, w->from);
	    strcat(buf, "\n");
	}
	if(w->date[0] && !ismc) {
	    if(lines & mailIsHtml)
		strcat(buf, "<br>");
	    lines = true;
	    strcat(buf, "Mail sent ");
	    strcat(buf, w->date);
	    strcat(buf, "\n");
	}
	if(w->reply[0]) {
	    if(lines & mailIsHtml)
		strcat(buf, "<br>");
	    lines = true;
	    strcat(buf, "Reply to ");
	    strcat(buf, w->reply);
	    strcat(buf, "\n");
	}
    }

    if(lines)
	strcat(buf, mailIsHtml ? "<P>\n" : "\n");
    strcpy(lastsubject, w->subject);
    return buf;
}				/* headerShow */

/* Depth first block of text determines the type */
static int
mailTextType(struct MHINFO *w)
{
    struct MHINFO *v;
    int texttype = CT_OTHER, rc;

    if(w->doAttach)
	return CT_OTHER;

/* jump right into the hard part, multi/alt */
    if(w->ct >= CT_MULTI) {
	foreach(v, w->components) {
	    rc = mailTextType(v);
	    if(rc == CT_HTML)
		return rc;
	    if(rc == CT_OTHER)
		continue;
	    if(w->ct == CT_MULTI)
		return rc;
	    texttype = rc;
	}
	return texttype;
    }

    /* multi */
    /* If there is no text, return nothing */
    if(w->start == w->end)
	return CT_OTHER;
/* I don't know if this is right, but I override the type,
 * and make it html, if we start out with <html> */
    if(memEqualCI(w->start, "<html>", 6))
	return CT_HTML;
    return w->ct == CT_HTML ? CT_HTML : CT_TEXT;
}				/* mailTextType */

static void
formatMail(struct MHINFO *w, bool top)
{
    struct MHINFO *v;
    int ct = w->ct;
    int j, best;

    if(w->doAttach)
	return;
    debugPrint(5, "format headers for content %d subject %s", ct, w->subject);
    stringAndString(&fm, &fm_l, headerShow(w, top));

    if(ct < CT_MULTI) {
	char *start = w->start;
	char *end = w->end;
	int newlen;
/* If mail is not in html, reformat it */
	if(start < end) {
	    if(ct == CT_TEXT) {
		breakLineSetup();
		if(breakLine(start, end - start, &newlen)) {
		    start = replaceLine;
		    end = start + newlen;
		}
	    }
	    if(mailIsHtml && ct != CT_HTML)
		stringAndString(&fm, &fm_l, "<pre>");
	    stringAndBytes(&fm, &fm_l, start, end - start);
	    if(mailIsHtml && ct != CT_HTML)
		stringAndString(&fm, &fm_l, "</pre>\n");
	}

	/* text present */
	/* There could be a mail message inline */
	foreach(v, w->components) {
	    if(end > start)
		stringAndString(&fm, &fm_l, mailIsHtml ? "<P>\n" : "\n");
	    formatMail(v, false);
	}

	return;
    }

    if(ct == CT_MULTI) {
	foreach(v, w->components)
	   formatMail(v, false);
	return;
    }

/* alternate presentations, pick the best one */
    best = j = 0;
    foreach(v, w->components) {
	int subtype = mailTextType(v);
	++j;
	if(subtype != CT_OTHER)
	    best = j;
	if(mailIsHtml && subtype == CT_HTML ||
	   !mailIsHtml && subtype == CT_TEXT)
	    break;
    }

    if(!best)
	best = 1;
    j = 0;
    foreach(v, w->components) {
	++j;
	if(j != best)
	    continue;
	formatMail(v, false);
	break;
    }
}				/* formatMail */

/* Browse the email file. */
char *
emailParse(char *buf)
{
    struct MHINFO *w, *v;
    nattach = nimages = 0;
    firstAttach = 0;
    mailIsHtml = mailIsSpam = ignoreImages = false;
    fm = initString(&fm_l);
    w = headerGlean(buf, buf + strlen(buf));
    mailIsHtml = (mailTextType(w) == CT_HTML);
    if(w->ne)
	mailIsSpam = true;
    else if(w->ct == CT_ALT) {
	foreach(v, w->components)
	   if(v->ne)
	    mailIsSpam = true;
    }
    if(mailIsHtml)
	stringAndString(&fm, &fm_l, "<html>\n");
    formatMail(w, true);
/* Remember, we always need a nonzero buffer */
    if(!fm_l || fm[fm_l - 1] != '\n')
	stringAndChar(&fm, &fm_l, '\n');
    if(!ismc) {
	writeAttachments(w);
	freeMailInfo(w);
	nzFree(buf);
    } else {
	lastMailInfo = w;
	lastMailText = buf;
    }
    return fm;
}				/* emailParse */
