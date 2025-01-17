/*	$OpenBSD: ctfdump.c,v 1.26 2022/08/10 07:58:04 tb Exp $ */

/*
 * Copyright (c) 2016 Martin Pieuchot <mpi@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ctf.h>

#include <err.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <locale.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef ZLIB
#include <zlib.h>
#endif /* ZLIB */

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

#define DUMP_OBJECT	(1 << 0)
#define DUMP_FUNCTION	(1 << 1)
#define DUMP_HEADER	(1 << 2)
#define DUMP_LABEL	(1 << 3)
#define DUMP_STRTAB	(1 << 4)
#define DUMP_STATISTIC	(1 << 5)
#define DUMP_TYPE	(1 << 6)

int		 dump(const char *, uint8_t);
int		 isctf(const char *, size_t);
__dead void	 usage(void);

int		 ctf_dump(const char *, size_t, uint8_t);
void		 ctf_dump_type(struct ctf_header *, const char *, off_t,
		     uint32_t, uint32_t *, uint32_t);
const char	*ctf_kind2name(uint16_t);
const char	*ctf_enc2name(uint16_t);
const char	*ctf_fpenc2name(uint16_t);
const char	*ctf_off2name(struct ctf_header *, const char *, off_t,
		     uint32_t);

char		*decompress(const char *, size_t, off_t);
int		 elf_dump(uint8_t);
const char	*elf_idx2sym(size_t *, uint8_t);

int
main(int argc, char *argv[])
{
	const char *filename;
	uint8_t flags = 0;
	int ch, error = 0;

	setlocale(LC_ALL, "");

	if (pledge("stdio rpath", NULL) == -1)
		err(1, "pledge");

	while ((ch = getopt(argc, argv, "dfhlst")) != -1) {
		switch (ch) {
		case 'd':
			flags |= DUMP_OBJECT;
			break;
		case 'f':
			flags |= DUMP_FUNCTION;
			break;
		case 'h':
			flags |= DUMP_HEADER;
			break;
		case 'l':
			flags |= DUMP_LABEL;
			break;
		case 's':
			flags |= DUMP_STRTAB;
			break;
		case 't':
			flags |= DUMP_TYPE;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc <= 0)
		usage();

	/* Dump everything by default */
	if (flags == 0)
		flags = 0xff;

	if (elf_version(EV_CURRENT) == EV_NONE)
		errx(1, "elf_version: %s", elf_errmsg(-1));

	while ((filename = *argv++) != NULL)
		error |= dump(filename, flags);

	return error;
}

Elf	*e;
Elf_Scn	*scnsymtab;
size_t	 strtabndx, strtabsz, nsymb;

int
dump(const char *path, uint8_t flags)
{
	struct stat	 st;
	char		*p;
	int		 fd, error = 1;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		warn("open");
		return 1;
	}

	if ((e = elf_begin(fd, ELF_C_READ, NULL)) == NULL) {
		warnx("elf_begin: %s", elf_errmsg(-1));
		goto done;
	}

	if (elf_kind(e) == ELF_K_ELF) {
		error = elf_dump(flags);
		elf_end(e);
		goto done;
	}
	elf_end(e);

	if (fstat(fd, &st) == -1) {
		warn("fstat");
		goto done;
	}
	if ((uintmax_t)st.st_size > SIZE_MAX) {
		warnx("file too big to fit memory");
		goto done;
	}

	p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED)
		err(1, "mmap");

	if (isctf(p, st.st_size))
		error = ctf_dump(p, st.st_size, flags);

	munmap(p, st.st_size);

 done:
	close(fd);
	return error;
}

const char *
elf_idx2sym(size_t *idx, uint8_t type)
{
	GElf_Sym	 sym;
	Elf_Data	*data;
	char		*name;
	size_t		 i;

	if (scnsymtab == NULL || strtabndx == 0)
		return NULL;

	data = NULL;
	while ((data = elf_rawdata(scnsymtab, data)) != NULL) {
		for (i = *idx + 1; i < nsymb; i++) {
			if (gelf_getsym(data, i, &sym) != &sym)
				continue;
			if (GELF_ST_TYPE(sym.st_info) != type)
				continue;
			if (sym.st_name >= strtabsz)
				break;
			if ((name = elf_strptr(e, strtabndx,
			    sym.st_name)) == NULL)
				continue;

			*idx = i;
			return name;
		}
	}

	return NULL;
}

int
elf_dump(uint8_t flags)
{
	GElf_Shdr	 shdr;
	Elf_Scn		*scn, *scnctf;
	Elf_Data	*data;
	char		*name;
	size_t		 shstrndx;
	int		 error = 0;

	if (elf_getshdrstrndx(e, &shstrndx) != 0) {
		warnx("elf_getshdrstrndx: %s", elf_errmsg(-1));
		return 1;
	}

	scn = scnctf = NULL;
	while ((scn = elf_nextscn(e, scn)) != NULL) {
		if (gelf_getshdr(scn, &shdr) != &shdr) {
			warnx("elf_getshdr: %s", elf_errmsg(-1));
			return 1;
		}

		if ((name = elf_strptr(e, shstrndx, shdr.sh_name)) == NULL) {
			warnx("elf_strptr: %s", elf_errmsg(-1));
			return 1;
		}

		if (strcmp(name, ELF_CTF) == 0)
			scnctf = scn;

		if (strcmp(name, ELF_SYMTAB) == 0 &&
		    shdr.sh_type == SHT_SYMTAB && shdr.sh_entsize != 0) {
			scnsymtab = scn;
			nsymb = shdr.sh_size / shdr.sh_entsize;
		}

		if (strcmp(name, ELF_STRTAB) == 0 &&
		    shdr.sh_type == SHT_STRTAB) {
			strtabndx = elf_ndxscn(scn);
			strtabsz = shdr.sh_size;
		}
	}

	if (scnctf == NULL) {
		warnx("%s section not found", ELF_CTF);
		return 1;
	}

	if (scnsymtab == NULL)
		warnx("symbol table not found");

	data = NULL;
	while ((data = elf_rawdata(scnctf, data)) != NULL) {
		if (data->d_buf == NULL) {
			warnx("%s section size is zero", ELF_CTF);
			return 1;
		}

		if (isctf(data->d_buf, data->d_size))
			error |= ctf_dump(data->d_buf, data->d_size, flags);
	}

	return error;
}

int
isctf(const char *p, size_t filesize)
{
	struct ctf_header	 cth;
	off_t			 dlen;

	if (filesize < sizeof(struct ctf_header)) {
		warnx("file too small to be CTF");
		return 0;
	}

	memcpy(&cth, p, sizeof(struct ctf_header));
	if (cth.cth_magic != CTF_MAGIC || cth.cth_version != CTF_VERSION)
		return 0;

	dlen = (off_t)cth.cth_stroff + cth.cth_strlen;
	if (dlen > (off_t)filesize && !(cth.cth_flags & CTF_F_COMPRESS)) {
		warnx("bogus file size");
		return 0;
	}

	if ((cth.cth_lbloff & 3) || (cth.cth_objtoff & 1) ||
	    (cth.cth_funcoff & 1) || (cth.cth_typeoff & 3)) {
		warnx("wrongly aligned offset");
		return 0;
	}

	if ((cth.cth_lbloff >= dlen) || (cth.cth_objtoff >= dlen) ||
	    (cth.cth_funcoff >= dlen) || (cth.cth_typeoff >= dlen)) {
		warnx("truncated file");
		return 0;
	}

	if ((cth.cth_lbloff > cth.cth_objtoff) ||
	    (cth.cth_objtoff > cth.cth_funcoff) ||
	    (cth.cth_funcoff > cth.cth_typeoff) ||
	    (cth.cth_typeoff > cth.cth_stroff)) {
		warnx("corrupted file");
		return 0;
	}

	return 1;
}

int
ctf_dump(const char *p, size_t size, uint8_t flags)
{
	struct ctf_header	 cth;
	off_t			 dlen;
	char			*data;

	memcpy(&cth, p, sizeof(struct ctf_header));
	dlen = (off_t)cth.cth_stroff + cth.cth_strlen;
	if (cth.cth_flags & CTF_F_COMPRESS) {
		data = decompress(p + sizeof(cth), size - sizeof(cth), dlen);
		if (data == NULL)
			return 1;
	} else {
		data = (char *)p + sizeof(cth);
	}

	if (flags & DUMP_HEADER) {
		printf("  cth_magic    = 0x%04x\n", cth.cth_magic);
		printf("  cth_version  = %u\n", cth.cth_version);
		printf("  cth_flags    = 0x%02x\n", cth.cth_flags);
		printf("  cth_parlabel = %s\n",
		    ctf_off2name(&cth, data, dlen, cth.cth_parlabel));
		printf("  cth_parname  = %s\n",
		    ctf_off2name(&cth, data, dlen, cth.cth_parname));
		printf("  cth_lbloff   = %u\n", cth.cth_lbloff);
		printf("  cth_objtoff  = %u\n", cth.cth_objtoff);
		printf("  cth_funcoff  = %u\n", cth.cth_funcoff);
		printf("  cth_typeoff  = %u\n", cth.cth_typeoff);
		printf("  cth_stroff   = %u\n", cth.cth_stroff);
		printf("  cth_strlen   = %u\n", cth.cth_strlen);
		printf("\n");
	}

	if (flags & DUMP_LABEL) {
		uint32_t		 lbloff = cth.cth_lbloff;
		struct ctf_lblent	*ctl;

		while (lbloff < cth.cth_objtoff) {
			ctl = (struct ctf_lblent *)(data + lbloff);

			printf("  %5u %s\n", ctl->ctl_typeidx,
			    ctf_off2name(&cth, data, dlen, ctl->ctl_label));

			lbloff += sizeof(*ctl);
		}
		printf("\n");
	}

	if (flags & DUMP_OBJECT) {
		uint32_t		 objtoff = cth.cth_objtoff;
		size_t			 idx = 0, i = 0;
		uint16_t		*dsp;
		const char		*s;
		int			 l;

		while (objtoff < cth.cth_funcoff) {
			dsp = (uint16_t *)(data + objtoff);

			l = printf("  [%zu] %u", i++, *dsp);
			if ((s = elf_idx2sym(&idx, STT_OBJECT)) != NULL)
				printf("%*s %s (%zu)\n", (14 - l), "", s, idx);
			else
				printf("\n");

			objtoff += sizeof(*dsp);
		}
		printf("\n");
	}

	if (flags & DUMP_FUNCTION) {
		uint16_t		*fsp, kind, vlen;
		uint16_t		*fstart, *fend;
		size_t			 idx = 0, i = -1;
		const char		*s;
		int			 l;

		fstart = (uint16_t *)(data + cth.cth_funcoff);
		fend = (uint16_t *)(data + cth.cth_typeoff);

		fsp = fstart;
		while (fsp < fend) {
			kind = CTF_INFO_KIND(*fsp);
			vlen = CTF_INFO_VLEN(*fsp);
			s = elf_idx2sym(&idx, STT_FUNC);
			fsp++;
			i++;

			if (kind == CTF_K_UNKNOWN && vlen == 0)
				continue;

			l = printf("  [%zu] FUNC ", i);
			if (s != NULL)
				printf("(%s) ", s);
			printf("returns: %u args: (", *fsp++);
			while (vlen-- > 0 && fsp < fend)
				printf("%u%s", *fsp++, (vlen > 0) ? ", " : "");
			printf(")\n");
		}
		printf("\n");
	}

	if (flags & DUMP_TYPE) {
		uint32_t		 idx = 1, offset = cth.cth_typeoff;
		uint32_t		 stroff = cth.cth_stroff;

		while (offset < stroff) {
			ctf_dump_type(&cth, data, dlen, stroff, &offset, idx++);
		}
		printf("\n");
	}

	if (flags & DUMP_STRTAB) {
		uint32_t		 offset = 0;
		const char		*str;

		while (offset < cth.cth_strlen) {
			str = ctf_off2name(&cth, data, dlen, offset);

			printf("  [%u] ", offset);
			if (strcmp(str, "(anon)"))
				offset += printf("%s\n", str);
			else {
				printf("\\0\n");
				offset++;
			}
		}
		printf("\n");
	}

	if (cth.cth_flags & CTF_F_COMPRESS)
		free(data);

	return 0;
}

void
ctf_dump_type(struct ctf_header *cth, const char *data, off_t dlen,
    uint32_t stroff, uint32_t *offset, uint32_t idx)
{
	const char		*p = data + *offset;
	const struct ctf_type	*ctt = (struct ctf_type *)p;
	const struct ctf_array	*cta;
	uint16_t		*argp, i, kind, vlen, root;
	uint32_t		 eob, toff;
	uint64_t		 size;
	const char		*name, *kname;

	kind = CTF_INFO_KIND(ctt->ctt_info);
	vlen = CTF_INFO_VLEN(ctt->ctt_info);
	root = CTF_INFO_ISROOT(ctt->ctt_info);
	name = ctf_off2name(cth, data, dlen, ctt->ctt_name);

	if (root)
		printf("  <%u> ", idx);
	else
		printf("  [%u] ", idx);

	if ((kname = ctf_kind2name(kind)) != NULL)
		printf("%s %s", kname, name);

	if (ctt->ctt_size <= CTF_MAX_SIZE) {
		size = ctt->ctt_size;
		toff = sizeof(struct ctf_stype);
	} else {
		size = CTF_TYPE_LSIZE(ctt);
		toff = sizeof(struct ctf_type);
	}

	switch (kind) {
	case CTF_K_UNKNOWN:
	case CTF_K_FORWARD:
		break;
	case CTF_K_INTEGER:
		eob = *((uint32_t *)(p + toff));
		toff += sizeof(uint32_t);
		printf(" encoding=%s offset=%u bits=%u",
		    ctf_enc2name(CTF_INT_ENCODING(eob)), CTF_INT_OFFSET(eob),
		    CTF_INT_BITS(eob));
		break;
	case CTF_K_FLOAT:
		eob = *((uint32_t *)(p + toff));
		toff += sizeof(uint32_t);
		printf(" encoding=%s offset=%u bits=%u",
		    ctf_fpenc2name(CTF_FP_ENCODING(eob)), CTF_FP_OFFSET(eob),
		    CTF_FP_BITS(eob));
		break;
	case CTF_K_ARRAY:
		cta = (struct ctf_array *)(p + toff);
		printf(" content: %u index: %u nelems: %u\n", cta->cta_contents,
		    cta->cta_index, cta->cta_nelems);
		toff += sizeof(struct ctf_array);
		break;
	case CTF_K_FUNCTION:
		argp = (uint16_t *)(p + toff);
		printf(" returns: %u args: (%u", ctt->ctt_type, *argp);
		for (i = 1; i < vlen; i++) {
			argp++;
			if ((const char *)argp > data + dlen)
				errx(1, "offset exceeds CTF section");

			printf(", %u", *argp);
		}
		printf(")");
		toff += (vlen + (vlen & 1)) * sizeof(uint16_t);
		break;
	case CTF_K_STRUCT:
	case CTF_K_UNION:
		printf(" (%llu bytes)\n", size);

		if (size < CTF_LSTRUCT_THRESH) {
			for (i = 0; i < vlen; i++) {
				struct ctf_member	*ctm;

				if (p + toff > data + dlen)
					errx(1, "offset exceeds CTF section");

				if (toff > (stroff - sizeof(*ctm)))
					break;

				ctm = (struct ctf_member *)(p + toff);
				toff += sizeof(struct ctf_member);

				printf("\t%s type=%u off=%u\n",
				    ctf_off2name(cth, data, dlen,
					ctm->ctm_name),
				    ctm->ctm_type, ctm->ctm_offset);
			}
		} else {
			for (i = 0; i < vlen; i++) {
				struct ctf_lmember	*ctlm;

				if (p + toff > data + dlen)
					errx(1, "offset exceeds CTF section");

				if (toff > (stroff - sizeof(*ctlm)))
					break;

				ctlm = (struct ctf_lmember *)(p + toff);
				toff += sizeof(struct ctf_lmember);

				printf("\t%s type=%u off=%llu\n",
				    ctf_off2name(cth, data, dlen,
					ctlm->ctlm_name),
				    ctlm->ctlm_type, CTF_LMEM_OFFSET(ctlm));
			}
		}
		break;
	case CTF_K_ENUM:
		printf("\n");
		for (i = 0; i < vlen; i++) {
			struct ctf_enum	*cte;

			if (p + toff > data + dlen)
				errx(1, "offset exceeds CTF section");

			if (toff > (stroff - sizeof(*cte)))
				break;

			cte = (struct ctf_enum *)(p + toff);
			toff += sizeof(struct ctf_enum);

			printf("\t%s = %d\n",
			    ctf_off2name(cth, data, dlen, cte->cte_name),
			    cte->cte_value);
		}
		break;
	case CTF_K_POINTER:
	case CTF_K_TYPEDEF:
	case CTF_K_VOLATILE:
	case CTF_K_CONST:
	case CTF_K_RESTRICT:
		printf(" refers to %u", ctt->ctt_type);
		break;
	default:
		errx(1, "incorrect type %u at offset %u", kind, *offset);
	}

	printf("\n");

	*offset += toff;
}

const char *
ctf_kind2name(uint16_t kind)
{
	static const char *kind_name[] = { NULL, "INTEGER", "FLOAT", "POINTER",
	   "ARRAY", "FUNCTION", "STRUCT", "UNION", "ENUM", "FORWARD",
	   "TYPEDEF", "VOLATILE", "CONST", "RESTRICT" };

	if (kind >= nitems(kind_name))
		return NULL;

	return kind_name[kind];
}

const char *
ctf_enc2name(uint16_t enc)
{
	static const char *enc_name[] = { "SIGNED", "CHAR", "SIGNED CHAR",
	    "BOOL", "SIGNED BOOL" };
	static char invalid[7];

	if (enc == CTF_INT_VARARGS)
		return "VARARGS";

	if (enc > 0 && enc <= nitems(enc_name))
		return enc_name[enc - 1];

	snprintf(invalid, sizeof(invalid), "0x%x", enc);
	return invalid;
}

const char *
ctf_fpenc2name(uint16_t enc)
{
	static const char *enc_name[] = { "SINGLE", "DOUBLE", NULL, NULL,
	    NULL, "LDOUBLE" };
	static char invalid[7];

	if (enc > 0 && enc <= nitems(enc_name) && enc_name[enc - 1] != NULL)
		return enc_name[enc - 1];

	snprintf(invalid, sizeof(invalid), "0x%x", enc);
	return invalid;
}

const char *
ctf_off2name(struct ctf_header *cth, const char *data, off_t dlen,
    uint32_t offset)
{
	const char		*name;

	if (CTF_NAME_STID(offset) != CTF_STRTAB_0)
		return "external";

	if (CTF_NAME_OFFSET(offset) >= cth->cth_strlen)
		return "exceeds strlab";

	if (cth->cth_stroff + CTF_NAME_OFFSET(offset) >= dlen)
		return "invalid";

	name = data + cth->cth_stroff + CTF_NAME_OFFSET(offset);
	if (*name == '\0')
		return "(anon)";

	return name;
}

char *
decompress(const char *buf, size_t size, off_t len)
{
#ifdef ZLIB
	z_stream		 stream;
	char			*data;
	int			 error;

	data = malloc(len);
	if (data == NULL) {
		warn(NULL);
		return NULL;
	}

	memset(&stream, 0, sizeof(stream));
	stream.next_in = (void *)buf;
	stream.avail_in = size;
	stream.next_out = (uint8_t *)data;
	stream.avail_out = len;

	if ((error = inflateInit(&stream)) != Z_OK) {
		warnx("zlib inflateInit failed: %s", zError(error));
		goto exit;
	}

	if ((error = inflate(&stream, Z_FINISH)) != Z_STREAM_END) {
		warnx("zlib inflate failed: %s", zError(error));
		inflateEnd(&stream);
		goto exit;
	}

	if ((error = inflateEnd(&stream)) != Z_OK) {
		warnx("zlib inflateEnd failed: %s", zError(error));
		goto exit;
	}

	if (len < 0 || (uintmax_t)stream.total_out != (uintmax_t)len) {
		warnx("decompression failed: %lu != %lld",
		    stream.total_out, len);
		goto exit;
	}

	return data;

exit:
	free(data);
#endif /* ZLIB */
	return NULL;
}

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s [-dfhlst] file ...\n",
	    getprogname());
	exit(1);
}
