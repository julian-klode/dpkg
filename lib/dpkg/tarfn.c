/*
 * libdpkg - Debian packaging suite library routines
 * tarfn.c - tar archive extraction functions
 *
 * Copyright © 1995 Bruce Perens
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <compat.h>

#include <errno.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include <dpkg/macros.h>
#include <dpkg/dpkg.h>
#include <dpkg/tarfn.h>

#define TAR_MAGIC_USTAR "ustar\0" "00"
#define TAR_MAGIC_GNU   "ustar "  " \0"

struct TarHeader {
	char Name[100];
	char Mode[8];
	char UserID[8];
	char GroupID[8];
	char Size[12];
	char ModificationTime[12];
	char Checksum[8];
	char LinkFlag;
	char LinkName[100];
	char MagicNumber[8];
	char UserName[32];
	char GroupName[32];
	char MajorDevice[8];
	char MinorDevice[8];
	char Prefix[155];	/* Only valid on ustar. */
};

static const size_t TarChecksumOffset = offsetof(struct TarHeader, Checksum);

/* Octal-ASCII-to-long */
static long
OtoL(const char *s, int size)
{
	int n = 0;

	while (*s == ' ') {
		s++;
		size--;
	}

	while (--size >= 0 && *s >= '0' && *s <= '7')
		n = (n * 010) + (*s++ - '0');

	return n;
}

/* String block to C null-terminated string */
static char *
StoC(const char *s, int size)
{
	int len;
	char *str;

	len = strnlen(s, size);
	str = m_malloc(len + 1);
	memcpy(str, s, len);
	str[len] = '\0';

	return str;
}

/* FIXME: Rewrite using varbuf, once it supports the needed functionality. */
static char *
get_prefix_name(struct TarHeader *h)
{
	char *prefix, *name, *s;

	/* The size is not going to be bigger than that. */
	s = m_malloc(257);

	prefix = StoC(h->Prefix, sizeof(h->Prefix));
	name = StoC(h->Name, sizeof(h->Name));

	strcpy(s, prefix);
	strcat(s, "/");
	strcat(s, name);

	free(prefix);
	free(name);

	return s;
}

static int
DecodeTarHeader(char *block, struct TarInfo *d)
{
	struct TarHeader *h = (struct TarHeader *)block;
	unsigned char *s = (unsigned char *)block;
	struct passwd *passwd = NULL;
	struct group *group = NULL;
	unsigned int i;
	long sum;
	long checksum;

	if (memcmp(h->MagicNumber, TAR_MAGIC_GNU, 6) == 0)
		d->format = tar_format_gnu;
	else if (memcmp(h->MagicNumber, TAR_MAGIC_USTAR, 6) == 0)
		d->format = tar_format_ustar;
	else
		d->format = tar_format_old;

	if (*h->UserName)
		passwd = getpwnam(h->UserName);
	if (*h->GroupName)
		group = getgrnam(h->GroupName);

	/* Concatenate prefix and name to support ustar style long names. */
	if (d->format == tar_format_ustar && h->Prefix[0] != '\0')
		d->Name = get_prefix_name(h);
	else
		d->Name = StoC(h->Name, sizeof(h->Name));
	d->LinkName = StoC(h->LinkName, sizeof(h->LinkName));
	d->Mode = (mode_t)OtoL(h->Mode, sizeof(h->Mode));
	d->Size = (size_t)OtoL(h->Size, sizeof(h->Size));
	d->ModTime = (time_t)OtoL(h->ModificationTime,
	                          sizeof(h->ModificationTime));
	d->Device = ((OtoL(h->MajorDevice,
	                   sizeof(h->MajorDevice)) & 0xff) << 8) |
	            (OtoL(h->MinorDevice, sizeof(h->MinorDevice)) & 0xff);
	checksum = OtoL(h->Checksum, sizeof(h->Checksum));
	d->UserID = (uid_t)OtoL(h->UserID, sizeof(h->UserID));
	d->GroupID = (gid_t)OtoL(h->GroupID, sizeof(h->GroupID));
	d->Type = (enum TarFileType)h->LinkFlag;

	if (passwd)
		d->UserID = passwd->pw_uid;

	if (group)
		d->GroupID = group->gr_gid;

	/* Treat checksum field as all blank. */
	sum = ' ' * sizeof(h->Checksum);
	for (i = TarChecksumOffset; i > 0; i--)
		sum += *s++;
	/* Skip the real checksum field. */
	s += sizeof(h->Checksum);
	for (i = (TARBLKSZ - TarChecksumOffset - sizeof(h->Checksum));
	     i > 0; i--)
		sum += *s++;

	return (sum == checksum);
}

struct symlinkList {
	struct TarInfo h;
	struct symlinkList *next;
};

int
TarExtractor(void *ctx, const struct tar_operations *ops)
{
	int status;
	char buffer[TARBLKSZ];
	struct TarInfo h;

	char *next_long_name, *next_long_link;
	char *bp;
	char **longp;
	int long_read;
	struct symlinkList *symlink_head, *symlink_tail, *symlink_node;

	next_long_name = NULL;
	next_long_link = NULL;
	symlink_tail = symlink_head = NULL;

	h.Name = NULL;
	h.LinkName = NULL;

	while ((status = ops->read(ctx, buffer, TARBLKSZ)) == TARBLKSZ) {
		int nameLength;

		if (!DecodeTarHeader(buffer, &h)) {
			if (h.Name[0] == '\0') {
				/* End of tape. */
				status = 0;
			} else {
				/* Indicates broken tarfile:
				 * “Header checksum error”. */
				errno = 0;
				status = -1;
			}
			break;
		}
		if (h.Type != GNU_LONGLINK && h.Type != GNU_LONGNAME) {
			if (next_long_name)
				h.Name = next_long_name;

			if (next_long_link)
				h.LinkName = next_long_link;

			next_long_link = NULL;
			next_long_name = NULL;
		}

		if (h.Name[0] == '\0') {
			/* Indicates broken tarfile: “Bad header data”. */
			errno = 0;
			status = -1;
			break;
		}

		nameLength = strlen(h.Name);

		switch (h.Type) {
		case NormalFile0:
		case NormalFile1:
			/* Compatibility with pre-ANSI ustar. */
			if (h.Name[nameLength - 1] != '/') {
				status = ops->extract_file(ctx, &h);
				break;
			}
			/* Else, fall through. */
		case Directory:
			if (h.Name[nameLength - 1] == '/') {
				h.Name[nameLength - 1] = '\0';
			}
			status = ops->mkdir(ctx, &h);
			break;
		case HardLink:
			status = ops->link(ctx, &h);
			break;
		case SymbolicLink:
			symlink_node = m_malloc(sizeof(*symlink_node));
			memcpy(&symlink_node->h, &h, sizeof(struct TarInfo));
			symlink_node->h.Name = m_strdup(h.Name);
			symlink_node->h.LinkName = m_strdup(h.LinkName);
			symlink_node->next = NULL;

			if (symlink_head)
				symlink_tail->next = symlink_node;
			else
				symlink_head = symlink_node;
			symlink_tail = symlink_node;
			status = 0;
			break;
		case CharacterDevice:
		case BlockDevice:
		case FIFO:
			status = ops->mknod(ctx, &h);
			break;
		case GNU_LONGLINK:
		case GNU_LONGNAME:
			/* Set longp to the location of the long filename or
			 * link we're trying to deal with. */
			longp = ((h.Type == GNU_LONGNAME) ?
			         &next_long_name :
			         &next_long_link);

			if (*longp)
				free(*longp);

			*longp = m_malloc(h.Size);
			bp = *longp;

			/* The way the GNU long{link,name} stuff works is like
			 * this:
			 *
			 * The first header is a “dummy” header that contains
			 *   the size of the filename.
			 * The next N headers contain the filename.
			 * After the headers with the filename comes the
			 *   “real” header with a bogus name or link. */
			for (long_read = h.Size;
			     long_read > 0;
			     long_read -= TARBLKSZ) {
				int copysize;

				status = ops->read(ctx, buffer, TARBLKSZ);
				/* If we didn't get TARBLKSZ bytes read, punt. */
				if (status != TARBLKSZ) {
					 /* Read partial header record? */
					if (status > 0) {
						errno = 0;
						status = -1;
					}
					break;
				}
				copysize = min(long_read, TARBLKSZ);
				memcpy (bp, buffer, copysize);
				bp += copysize;
			};

			/* In case of error do not overwrite status with 0. */
			if (status < 0)
				break;

			/* This decode function expects status to be 0 after
			 * the case statement if we successfully decoded. I
			 * guess what we just did was successful. */
			status = 0;
			break;
		default:
			/* Indicates broken tarfile: “Bad header field”. */
			errno = 0;
			status = -1;
		}
		if (status != 0)
			/* Pass on status from coroutine. */
			break;
	}

	while (symlink_head) {
		symlink_node = symlink_head->next;
		if (status == 0)
			status = ops->symlink(ctx, &symlink_head->h);
		free(symlink_head->h.Name);
		free(symlink_head->h.LinkName);
		free(symlink_head);
		symlink_head = symlink_node;
	}
	free(h.Name);
	free(h.LinkName);

	if (status > 0) {
		/* Indicates broken tarfile: “Read partial header record”. */
		errno = 0;
		return -1;
	} else {
		/* Return whatever I/O function returned. */
		return status;
	}
}

