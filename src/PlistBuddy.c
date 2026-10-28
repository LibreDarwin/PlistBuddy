/*
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * PlistBuddy - read and modify property list files.
 *
 * Reimplementation of /usr/libexec/PlistBuddy.  Function names, the
 * command/state layouts and every quirk below follow Apple's binary (whose
 * local symbols survive stripping), so output, exit status and the saved
 * file match it byte for byte.  Quirks kept on purpose, all observable:
 *
 *  - ParseAtom counts the terminating NUL as a character, so a missing
 *    argument parses as "" rather than NULL.  "Set" alone therefore acts
 *    on the working container instead of printing "Entry Required".
 *  - Every editing command marks the plist dirty even when it fails, so
 *    -c mode saves the file anyway.
 *  - Files are always written back as XML, whatever format they were in.
 *  - "real" values are stored as 32-bit floats; integers go through
 *    sscanf("%lld"), so "0x10" becomes 0.
 *  - Usage prints argv[0] from one past the last '/', and from argv[0]+1
 *    when there is no slash ("PlistBuddy" -> "listBuddy").
 *  - Copy makes shallow copies of containers.
 *
 * Deliberate differences, none reachable in normal use:
 *  - Apple keeps -c commands in a fixed 15-slot stack array and smashes
 *    its stack past 14 of them; we size the array from argc.
 *  - Import of a file into an existing string entry reads the file bytes
 *    as a C string; Apple's copy is not NUL-terminated, ours is.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Not in the public SDK headers. */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_INSTALLER 0x00000008

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

enum {
	kCmdNone, kCmdHelp, kCmdExit, kCmdSave, kCmdRevert, kCmdClear,
	kCmdPrint, kCmdSet, kCmdAdd, kCmdCopy, kCmdDelete, kCmdMerge,
	kCmdImport, kCmdCD
};

enum {
	kTypeNone, kTypeString, kTypeArray, kTypeDict, kTypeBool, kTypeReal,
	kTypeInteger, kTypeDate, kTypeData
};

/* (type & 0xe) == 2 selects array and dict, as Apple's code tests it. */
#define IS_CONTAINER(t) (((t) & 0xe) == 2)

typedef struct {
	int cmd;
	int type;
	char *entry;
	char *entry2;
	size_t valueLen;
	char *value;
	const char *flags;	/* Merge: "r" for -r, never freed */
} Command;

typedef struct {
	int xml;
	bool running;
	bool dirty;
	bool noFollow;
	CFPropertyListRef root;
	CFTypeRef working;
	char *path;
} State;

static char returnCode;

static void PrintEntryReadable(const char *fmt, CFTypeRef v);

static Command *
CreateCommand(void)
{
	return calloc(1, sizeof(Command));
}

static void
DisposeCommand(Command *c)
{
	free(c->entry);
	free(c->entry2);
	free(c->value);
	free(c);
}

/* ---------------------------------------------------------------- dates */

static char *
GetAbsoluteTimeString(CFAbsoluteTime t)
{
	CFTimeZoneRef tz = CFTimeZoneCopyDefault();
	CFGregorianDate gd = CFAbsoluteTimeGetGregorianDate(t, tz);
	struct tm tm = { 0 };

	tm.tm_year = gd.year - 1900;
	tm.tm_mon = gd.month - 1;
	tm.tm_mday = gd.day;
	tm.tm_wday = CFAbsoluteTimeGetDayOfWeek(t, tz) % 7;
	tm.tm_min = gd.minute;
	tm.tm_hour = gd.hour;
	tm.tm_sec = (int)gd.second;
	char *s = malloc(0x400);
	strftime(s, 0x400, "%a %b %d %H:%M:%S %Z %Y", &tm);
	CFRelease(tz);
	return s;
}

static CFAbsoluteTime
GetAbsoluteTimeFromString(const char *s)
{
	struct tm tm = { 0 };

	if (strptime(s, "%a %b %d %H:%M:%S %Z %Y", &tm) == NULL &&
	    strptime(s, "%c", &tm) == NULL &&
	    strptime(s, "%D", &tm) == NULL) {
		puts("Unrecognized Date Format");
		return 0.0;
	}
	CFGregorianDate gd;
	gd.year = tm.tm_year + 1900;
	gd.month = (SInt8)(tm.tm_mon + 1);
	gd.day = (SInt8)tm.tm_mday;
	gd.hour = (SInt8)tm.tm_hour;
	gd.minute = (SInt8)tm.tm_min;
	gd.second = (double)tm.tm_sec;
	CFTimeZoneRef tz = CFTimeZoneCopyDefault();
	CFAbsoluteTime t = CFGregorianDateGetAbsoluteTime(gd, tz);
	CFRelease(tz);
	return t;
}

/* ---------------------------------------------------------- entry paths */

/* Copy of name with "\:" turned into ":". */
static char *
FixUpName(const char *name)
{
	int len = (int)strlen(name);
	char *out = malloc(len + 1);
	int o = 0;

	for (int i = 0; i < len; i++, o++) {
		if (name[i] == '\\' && name[i + 1] == ':') {
			out[o] = ':';
			i++;
		} else {
			out[o] = name[i];
		}
	}
	out[o] = '\0';
	return out;
}

/* Last path component: text after the last ':' not escaped by '\'. */
static char *
GetEntryName(char *entry)
{
	for (int i = (int)strlen(entry) - 1; i >= 0; i--)
		if (entry[i] == ':' && (i == 0 || entry[i - 1] != '\\'))
			return entry + i + 1;
	return entry;
}

static int
PropertyTypeOfEntry(CFTypeRef v)
{
	CFTypeID t = CFGetTypeID(v);

	if (t == CFStringGetTypeID())
		return kTypeString;
	if (t == CFArrayGetTypeID())
		return kTypeArray;
	if (t == CFDictionaryGetTypeID())
		return kTypeDict;
	if (t == CFBooleanGetTypeID())
		return kTypeBool;
	if (t == CFNumberGetTypeID())
		return CFNumberIsFloatType(v) ? kTypeReal : kTypeInteger;
	if (t == CFDateGetTypeID())
		return kTypeDate;
	if (t == CFDataGetTypeID())
		return kTypeData;
	return kTypeNone;
}

/*
 * Child of container named by name.  With strict == 0 an empty name means
 * the container itself (or, for an array, its last element), and arrays
 * are indexed; with strict != 0 arrays never match, which is how Add
 * decides whether an entry "already exists".
 */
static CFTypeRef
GetEntryInPlist(CFTypeRef container, const char *name, int strict)
{
	if (name == NULL || (!strict && *name == '\0'))
		return container;

	CFTypeID t = CFGetTypeID(container);
	CFTypeRef v = NULL;

	if (t == CFDictionaryGetTypeID()) {
		CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault,
		    name, kCFStringEncodingUTF8);
		v = CFDictionaryGetValue(container, key);
		CFRelease(key);
	}
	if (t == CFArrayGetTypeID()) {
		v = NULL;
		if (!strict) {
			int idx = 0;
			if (*name == '\0')
				idx = (int)CFArrayGetCount(container) - 1;
			else
				sscanf(name, "%i", &idx);
			if (idx < CFArrayGetCount(container) && idx >= 0)
				v = CFArrayGetValueAtIndex(container, idx);
		}
	}
	return v;
}

static int
AddEntryToPlist(CFTypeRef container, CFTypeRef v, const char *name)
{
	CFTypeID t = CFGetTypeID(container);

	if (t == CFDictionaryGetTypeID()) {
		CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault,
		    name, kCFStringEncodingUTF8);
		CFDictionaryAddValue((CFMutableDictionaryRef)container, key, v);
		CFRelease(key);
		return 0;
	}
	if (t != CFArrayGetTypeID())
		return -1;
	if (name == NULL || *name == '\0') {
		CFArrayAppendValue((CFMutableArrayRef)container, v);
	} else {
		int idx = 0;
		sscanf(name, "%i", &idx);
		CFIndex count = CFArrayGetCount(container);
		if (count <= idx)
			idx = (int)count;
		CFArrayInsertValueAtIndex((CFMutableArrayRef)container, idx, v);
	}
	return 0;
}

static int
RemoveEntryFromPlist(CFTypeRef container, const char *name)
{
	CFTypeID t = CFGetTypeID(container);

	if (t == CFDictionaryGetTypeID()) {
		CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault,
		    name, kCFStringEncodingUTF8);
		CFDictionaryRemoveValue((CFMutableDictionaryRef)container, key);
		CFRelease(key);
		return 0;
	}
	if (t != CFArrayGetTypeID())
		return -1;
	int idx = 0;
	sscanf(name, "%i", &idx);
	int count = (int)CFArrayGetCount(container);
	if (idx >= count)
		idx = count - 1;
	if (count >= 1)
		CFArrayRemoveValueAtIndex((CFMutableArrayRef)container, idx);
	return 0;
}

/*
 * Container that holds the last component of entry.  Entries without a
 * leading path resolve against the working container, ":x" against the
 * root.  With create != 0, missing intermediate entries become dicts.
 */
static CFTypeRef
ResolveEntryParent(CFTypeRef root, CFTypeRef working, char *entry, int create)
{
	char *name = GetEntryName(entry);
	size_t elen = strlen(entry);
	int plen = (int)(elen - strlen(name));

	if (plen <= 0)
		return working;
	if (plen == 1 && *entry == ':')
		return root;

	char *parentPath = malloc(plen);
	memcpy(parentPath, entry, plen - 1);
	parentPath[plen - 1] = '\0';

	CFTypeRef result = NULL;
	CFTypeRef grand = ResolveEntryParent(root, working, parentPath, create);
	if (grand != NULL) {
		char *pname = FixUpName(GetEntryName(parentPath));
		result = GetEntryInPlist(grand, pname, 0);
		if (create && result == NULL) {
			CFMutableDictionaryRef d = CFDictionaryCreateMutable(
			    kCFAllocatorDefault, 0,
			    &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			result = AddEntryToPlist(grand, d, pname) == 0 ? d : NULL;
			CFRelease(d);
		}
		free(pname);
	}
	free(parentPath);
	return result;
}

static CFTypeRef
ResolveEntryStringToRef(State *s, char *entry)
{
	CFTypeRef parent = ResolveEntryParent(s->root, s->working, entry, 0);
	char *name = FixUpName(GetEntryName(entry));
	CFTypeRef v = parent ? GetEntryInPlist(parent, name, 0) : NULL;

	free(name);
	return v;
}

/* ------------------------------------------------------- value creation */

/* Returns a +1 object, except booleans which are the shared constants. */
static CFTypeRef
CreateEntryWithValue(int type, CFIndex len, const char *value)
{
	switch (type) {
	case kTypeString:
		if (value == NULL)
			return NULL;
		return CFStringCreateWithCString(kCFAllocatorDefault, value,
		    kCFStringEncodingUTF8);
	case kTypeArray:
		return CFArrayCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeArrayCallBacks);
	case kTypeDict:
		return CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
	case kTypeBool:
		if (value == NULL)
			return NULL;
		if (strcasecmp(value, "true") == 0 ||
		    strcasecmp(value, "yes") == 0 ||
		    strcasecmp(value, "1") == 0)
			return kCFBooleanTrue;
		return kCFBooleanFalse;
	case kTypeReal: {
		float f = 0;
		CFNumberRef n;
		/* sscanf's EOF (-1) counts as success, as in Apple's test. */
		if (value != NULL && sscanf(value, "%f", &f) != 0 &&
		    (n = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType,
		    &f)) != NULL)
			return n;
		puts("Unrecognized Real Format");
		return NULL;
	}
	case kTypeInteger: {
		long long ll = 0;
		CFNumberRef n;
		if (value != NULL && sscanf(value, "%lld", &ll) != 0 &&
		    (n = CFNumberCreate(kCFAllocatorDefault,
		    kCFNumberLongLongType, &ll)) != NULL)
			return n;
		puts("Unrecognized Integer Format");
		return NULL;
	}
	case kTypeDate: {
		CFAbsoluteTime t;
		if (value == NULL || (t = GetAbsoluteTimeFromString(value)) == 0.0)
			return NULL;
		return CFDateCreate(kCFAllocatorDefault, t);
	}
	case kTypeData:
		if (value == NULL)
			return NULL;
		return CFDataCreate(kCFAllocatorDefault, (const UInt8 *)value,
		    (int)len);
	}
	return NULL;
}

/* Containers are copied shallowly; booleans are returned as is. */
static CFTypeRef
CreateCopyOfEntry(CFTypeRef v)
{
	CFTypeID t = CFGetTypeID(v);

	if (t == CFStringGetTypeID())
		return CFStringCreateCopy(kCFAllocatorDefault, v);
	if (t == CFArrayGetTypeID())
		return CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, v);
	if (t == CFDictionaryGetTypeID())
		return CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, v);
	if (t == CFBooleanGetTypeID())
		return v;
	if (t == CFNumberGetTypeID()) {
		char buf[16];
		CFNumberGetValue(v, CFNumberGetType(v), buf);
		return CFNumberCreate(kCFAllocatorDefault, CFNumberGetType(v),
		    buf);
	}
	if (t == CFDateGetTypeID())
		return CFDateCreate(kCFAllocatorDefault,
		    CFDateGetAbsoluteTime(v));
	if (t == CFDataGetTypeID())
		return CFDataCreateCopy(kCFAllocatorDefault, v);
	return NULL;
}

static int
AddMultipleEntriesToPlist(CFTypeRef dst, CFTypeRef src, int recursive)
{
	CFTypeID st = CFGetTypeID(src);
	CFTypeID dt = CFGetTypeID(dst);

	if (st == CFArrayGetTypeID() && dt == CFArrayGetTypeID()) {
		int n = (int)CFArrayGetCount(src);
		for (int i = 0; i < n; i++) {
			CFTypeRef v = CFArrayGetValueAtIndex(src, i);
			if (v != NULL)
				CFArrayAppendValue((CFMutableArrayRef)dst, v);
		}
		return 0;
	}
	if (st != CFDictionaryGetTypeID() || dt != CFDictionaryGetTypeID())
		return AddEntryToPlist(dst, src, "");

	int n = (int)CFDictionaryGetCount(src);
	const void **keys = malloc((long)n * sizeof(void *));
	const void **vals = malloc((long)n * sizeof(void *));
	CFDictionaryGetKeysAndValues(src, keys, vals);
	for (int i = 0; i < n; i++) {
		CFTypeRef cur = CFDictionaryGetValue(dst, keys[i]);
		if (cur == NULL) {
			CFDictionaryAddValue((CFMutableDictionaryRef)dst, keys[i],
			    vals[i]);
			continue;
		}
		if (recursive && CFGetTypeID(cur) == CFGetTypeID(vals[i]) &&
		    (CFGetTypeID(cur) == CFArrayGetTypeID() ||
		    CFGetTypeID(cur) == CFDictionaryGetTypeID())) {
			AddMultipleEntriesToPlist(cur, vals[i], 1);
			continue;
		}
		printf("Duplicate Entry Was Skipped: ");
		PrintEntryReadable("%s", keys[i]);
		putchar('\n');
	}
	free(keys);
	free(vals);
	return 0;
}

/* -------------------------------------------------------------- printing */

/*
 * fmt is "<indent>%s<suffix>": the indent prefixes nested lines and the
 * closing brace, the suffix ends each element line.
 */
static void
PrintEntryReadable(const char *fmt, CFTypeRef v)
{
	char indent[5] = "    ";
	char *prefix = calloc(1, strlen(fmt));
	memcpy(prefix, fmt, strstr(fmt, "%s") - fmt);
	const char *suffix = strstr(fmt, "%s") + 2;
	CFTypeID t = CFGetTypeID(v);

	if (t == CFStringGetTypeID()) {
		char *buf = malloc(0x1000);
		if (CFStringGetCString(v, buf, 0x1000, kCFStringEncodingUTF8))
			printf("%s", buf);
		free(buf);
	} else if (t == CFArrayGetTypeID()) {
		char *child = malloc(strlen(fmt) + 5);
		sprintf(child, "%s%s", indent, fmt);
		printf("Array {%s\n", suffix);
		int n = (int)CFArrayGetCount(v);
		for (int i = 0; i < n; i++) {
			printf("%s%s", indent, prefix);
			CFTypeRef e = CFArrayGetValueAtIndex(v, i);
			if (e != NULL)
				PrintEntryReadable(child, e);
			puts(suffix);
		}
		printf("%s}", prefix);
		free(child);
	} else if (t == CFDictionaryGetTypeID()) {
		char *child = malloc(strlen(fmt) + 5);
		sprintf(child, "%s%s", indent, fmt);
		printf("Dict {%s\n", suffix);
		int n = (int)CFDictionaryGetCount(v);
		const void **keys = malloc((long)n * sizeof(void *));
		const void **vals = malloc((long)n * sizeof(void *));
		CFDictionaryGetKeysAndValues(v, keys, vals);
		for (int i = 0; i < n; i++) {
			printf("%s%s", indent, prefix);
			PrintEntryReadable(child, keys[i]);
			printf(" = ");
			PrintEntryReadable(child, vals[i]);
			puts(suffix);
		}
		printf("%s}", prefix);
		free(keys);
		free(vals);
		free(child);
	} else if (t == CFBooleanGetTypeID()) {
		printf("%s", v == kCFBooleanTrue ? "true" : "false");
	} else if (t == CFNumberGetTypeID()) {
		if (CFNumberIsFloatType(v)) {
			float f = 0;
			CFNumberGetValue(v, kCFNumberFloatType, &f);
			printf("%f", (double)f);
		} else {
			long long ll = 0;
			CFNumberGetValue(v, kCFNumberLongLongType, &ll);
			printf("%lld", ll);
		}
	} else if (t == CFDateGetTypeID()) {
		char *s = GetAbsoluteTimeString(CFDateGetAbsoluteTime(v));
		if (s != NULL) {
			printf("%s", s);
			free(s);
		}
	} else if (t == CFDataGetTypeID()) {
		const char *p = (const char *)CFDataGetBytePtr(v);
		int len = (int)CFDataGetLength(v);
		for (const char *q = p; q < p + len; q++)
			putchar(*q);
	} else {
		printf("%s", "Unprintable Data");
	}
	free(prefix);
}

static void
PrintEntryXML(CFTypeRef v)
{
	CFDataRef d = CFPropertyListCreateXMLData(kCFAllocatorDefault, v);

	if (d == NULL)
		return;
	PrintEntryReadable("%s", d);
	CFRelease(d);
}

static void
PrintEntry(State *s, CFTypeRef v)
{
	if (s->xml) {
		PrintEntryXML(v);
	} else {
		PrintEntryReadable("%s", v);
		putchar('\n');
	}
}

/* ------------------------------------------------------------- file I/O */

static CFDataRef
ReadData(const char *path, int noFollow)
{
	int fd;
	struct stat st = { 0 };
	CFDataRef data = NULL;

	while ((fd = open(path, O_RDONLY | (noFollow ? O_NOFOLLOW_ANY : 0))) == -1) {
		if (errno != EINTR) {
			fprintf(stderr, "Error Opening File: %s [%s]\n", path,
			    strerror(errno));
			return NULL;
		}
	}
	if (fstat(fd, &st) != 0) {
		fprintf(stderr, "Error Querying File: %s [%s]\n", path,
		    strerror(errno));
		goto out;
	}
	char *buf = valloc(st.st_size);
	if (buf == NULL) {
		fprintf(stderr, "Error Allocating Memory for File: %s [%s]\n",
		    path, strerror(ENOMEM));
		goto out;
	}
	char *p = buf;
	for (size_t left = st.st_size; left != 0; ) {
		size_t chunk = left > 0xfffff ? 0x100000 : left;
		if ((size_t)read(fd, p, chunk) != chunk) {
			errno = ENODATA;
			fprintf(stderr, "Error Reading File: %s [%s]\n", path,
			    strerror(errno));
			free(buf);
			goto out;
		}
		left -= chunk;
		p += chunk;
	}
	data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (UInt8 *)buf,
	    st.st_size, kCFAllocatorMalloc);
out:
	if (close(fd) == -1)
		abort();
	return data;
}

static CFPropertyListRef
ReadPlist(const char *path, int noFollow)
{
	CFDataRef data = ReadData(path, noFollow);

	if (data == NULL)
		return NULL;
	CFStringRef err = NULL;
	CFPropertyListRef pl = CFPropertyListCreateFromXMLData(
	    kCFAllocatorDefault, data, kCFPropertyListMutableContainersAndLeaves,
	    &err);
	if (err != NULL) {
		CFShow(err);
		CFRelease(err);
	}
	CFRelease(data);
	return pl;
}

static void
SavePlist(CFPropertyListRef root, const char *path, int noFollow)
{
	int flags = O_WRONLY | O_CREAT | O_TRUNC | (noFollow ? O_NOFOLLOW_ANY : 0);
	int fd;

	while ((fd = open(path, flags, 0644)) == -1) {
		if (errno != EINTR) {
			fprintf(stderr, "Error Opening Destination: %s [%s]\n", path,
			    strerror(errno));
			return;
		}
	}
	CFErrorRef err = NULL;
	CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault, root,
	    kCFPropertyListXMLFormat_v1_0, 0, &err);
	if (data == NULL) {
		if (err == NULL) {
			fprintf(stderr,
			    "Error Converting Property List: %s [Unknown Error]\n",
			    path);
			goto out;
		}
		CFStringRef desc = CFErrorCopyDescription(err);
		if (desc != NULL) {
			fprintf(stderr, "Error Converting Property List: %s [%s]\n",
			    path, CFStringGetCStringPtr(desc,
			    kCFStringEncodingUTF8));
			CFRelease(desc);
		}
		CFRelease(err);
		goto out;
	}
	const UInt8 *p = CFDataGetBytePtr(data);
	for (size_t left = CFDataGetLength(data); left != 0; ) {
		ssize_t n;
		while ((n = write(fd, p, left)) < 0) {
			if (errno != EAGAIN && errno != EINTR) {
				fprintf(stderr,
				    "Error Writing to Destination: %s [%s]\n", path,
				    strerror(errno));
				goto release;
			}
		}
		p += n;
		left -= n;
	}
release:
	CFRelease(data);
out:
	if (close(fd) == -1)
		abort();
}

static int
RevertPlist(State *s)
{
	if (s->root != NULL) {
		CFRelease(s->root);
		s->root = NULL;
	}
	if (faccessat(AT_FDCWD, s->path, R_OK,
	    s->noFollow ? AT_SYMLINK_NOFOLLOW_ANY : 0) == 0) {
		s->root = s->working = ReadPlist(s->path, s->noFollow);
		if (s->root == NULL) {
			printf("Error Reading File: %s\n", s->path);
			return 0;
		}
	} else {
		printf("File Doesn't Exist, Will Create: %s\n", s->path);
		s->root = s->working = CFDictionaryCreateMutable(
		    kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
	}
	return 1;
}

/* -------------------------------------------------------------- commands */

/*
 * Insert v as the new entry, creating missing parents; shared by Add and
 * Copy, whose messages differ only in the leading verb.  Consumes v.
 */
static void
AddNewEntry(State *s, CFTypeRef v, char *entry, const char *verb)
{
	CFTypeRef parent = ResolveEntryParent(s->root, s->working, entry, 1);
	char *name = FixUpName(GetEntryName(entry));
	const char *err = NULL;

	if (parent == NULL)
		err = "%s: Entry, \"%s\", Incorrectly Specified\n";
	else if (GetEntryInPlist(parent, name, 1) != NULL)
		err = "%s: \"%s\" Entry Already Exists\n";
	else if (AddEntryToPlist(parent, v, name) != 0)
		err = "%s: Can't Add Entry, \"%s\", to Parent\n";
	if (err != NULL) {
		fprintf(stderr, err, verb, entry);
		returnCode = 1;
	}
	free(name);
	CFRelease(v);
}

static void
CommandAdd(State *s, Command *c)
{
	CFTypeRef v = CreateEntryWithValue(c->type, c->valueLen, c->value);

	if (v != NULL)
		AddNewEntry(s, v, c->entry, "Add");
}

static void
CommandCopy(State *s, Command *c)
{
	CFTypeRef src = ResolveEntryStringToRef(s, c->entry);
	if (src == NULL) {
		fprintf(stderr, "Copy: Entry, \"%s\", Does Not Exist\n", c->entry);
		returnCode = 1;
		return;
	}
	AddNewEntry(s, CreateCopyOfEntry(src), c->entry2, "Copy");
}

static void
CommandSet(State *s, Command *c)
{
	CFTypeRef parent = ResolveEntryParent(s->root, s->working, c->entry, 0);
	char *name = FixUpName(GetEntryName(c->entry));
	CFTypeRef cur = parent ? GetEntryInPlist(parent, name, 0) : NULL;

	if (cur == NULL) {
		fprintf(stderr, "Set: Entry, \"%s\", Does Not Exist\n", c->entry);
		returnCode = 1;
		goto out;
	}
	c->type = PropertyTypeOfEntry(cur);
	if (IS_CONTAINER(c->type)) {
		fputs("Set: Cannot Perform Set On Containers\n", stderr);
		returnCode = 1;
		goto out;
	}
	CFTypeRef v = CreateEntryWithValue(c->type, c->valueLen, c->value);
	if (v == NULL)
		goto out;
	if (cur == s->root) {
		CFRelease(s->root);
		s->root = s->working = v;
	} else {
		RemoveEntryFromPlist(parent, name);
		if (AddEntryToPlist(parent, v, name) != 0) {
			fputs("Set: Internal Error Occurred\n", stderr);
			returnCode = 1;
		}
		CFRelease(v);
	}
out:
	free(name);
}

static void
CommandPrint(State *s, Command *c)
{
	CFTypeRef v = ResolveEntryStringToRef(s, c->entry);

	if (v == NULL) {
		fprintf(stderr, "Print: Entry, \"%s\", Does Not Exist\n", c->entry);
		returnCode = 1;
		return;
	}
	PrintEntry(s, v);
}

static void
CommandDelete(State *s, Command *c)
{
	CFTypeRef parent = ResolveEntryParent(s->root, s->working, c->entry, 0);
	char *name = FixUpName(GetEntryName(c->entry));
	CFTypeRef cur = parent ? GetEntryInPlist(parent, name, 0) : NULL;

	if (cur == NULL) {
		fprintf(stderr, "Delete: Entry, \"%s\", Does Not Exist\n", c->entry);
		returnCode = 1;
	} else {
		if (s->working == cur) {
			puts("Working Container has become Invalid.  Setting to :");
			s->working = s->root;
		}
		RemoveEntryFromPlist(parent, name);
	}
	free(name);
}

static void
CommandMerge(State *s, Command *c)
{
	CFTypeRef dst;

	if (c->entry == NULL) {
		dst = s->working;
	} else {
		dst = ResolveEntryStringToRef(s, c->entry);
		if (dst == NULL) {
			fprintf(stderr, "Merge: \"%s\" Entry Does Not Exist\n",
			    c->entry);
			returnCode = 1;
			return;
		}
		c->type = PropertyTypeOfEntry(dst);
		if (!IS_CONTAINER(c->type)) {
			fputs("Merge: Specified Entry Must Be a Container\n", stderr);
			returnCode = 1;
			return;
		}
	}

	CFPropertyListRef src = ReadPlist(c->value, s->noFollow);
	if (src == NULL) {
		fprintf(stderr, "Merge: Error Reading File: %s\n", c->value);
		returnCode = 1;
		return;
	}
	int st = PropertyTypeOfEntry(src);
	if (st != c->type && st == kTypeDict) {
		fputs("Merge: Can't Add dict Entries to array\n", stderr);
		returnCode = 1;
	} else if (st != c->type && st == kTypeArray) {
		fputs("Merge: Can't Add array Entries to dict\n", stderr);
		returnCode = 1;
	} else {
		AddMultipleEntriesToPlist(dst, src,
		    c->flags != NULL && strchr(c->flags, 'r') != NULL);
	}
	CFRelease(src);
}

static void
CommandImport(State *s, Command *c)
{
	CFTypeRef cur = ResolveEntryStringToRef(s, c->entry);

	if (cur == NULL) {
		c->type = kTypeData;
	} else {
		c->type = PropertyTypeOfEntry(cur);
		if (IS_CONTAINER(c->type)) {
			fputs("Import: Specified Entry Must Not Be a Container\n",
			    stderr);
			returnCode = 1;
			return;
		}
	}

	CFDataRef data = ReadData(c->value, s->noFollow);
	if (data == NULL) {
		fprintf(stderr, "Import: Error Reading File: %s\n", c->value);
		returnCode = 1;
		return;
	}
	free(c->value);
	c->valueLen = CFDataGetLength(data);
	c->value = malloc(c->valueLen + 1);
	CFDataGetBytes(data, CFRangeMake(0, c->valueLen), (UInt8 *)c->value);
	c->value[c->valueLen] = '\0';
	CFRelease(data);
	if (cur == NULL)
		CommandAdd(s, c);
	else
		CommandSet(s, c);
}

static void
CommandCD(State *s, Command *c)
{
	CFTypeRef v = ResolveEntryStringToRef(s, c->entry);
	const char *err;

	if (v == NULL) {
		err = "CD: Entry, \"%s\", Does Not Exist\n";
	} else {
		c->type = PropertyTypeOfEntry(v);
		if (IS_CONTAINER(c->type)) {
			s->working = v;
			return;
		}
		err = "CD: Entry, \"%s\", Is Not A Container\n";
	}
	fprintf(stderr, err, c->entry);
	returnCode = 1;
}

static void
PrintHelp(void)
{
	puts("Command Format:");
	puts("    Help - Prints this information");
	puts("    Exit - Exits the program, changes are not saved to the file");
	puts("    Save - Saves the current changes to the file");
	puts("    Revert - Reloads the last saved version of the file");
	puts("    Clear [<Type>] - Clears out all existing entries, and creates root of Type");
	puts("    Print [<Entry>] - Prints value of Entry.  Otherwise, prints file");
	puts("    Set <Entry> <Value> - Sets the value at Entry to Value");
	puts("    Add <Entry> <Type> [<Value>] - Adds Entry to the plist, with value Value");
	puts("    Copy <EntrySrc> <EntryDst> - Copies the EntrySrc property to EntryDst");
	puts("    Delete <Entry> - Deletes Entry from the plist");
	puts("    Merge <file.plist> [<Entry>] - Adds the contents of file.plist to Entry");
	puts("    Import <Entry> <file> - Creates or sets Entry the contents of file");
	putchar('\n');
	puts("Entry Format:");
	puts("    Entries consist of property key names delimited by colons.  Array items");
	puts("    are specified by a zero-based integer index.  Examples:");
	puts("        :CFBundleShortVersionString");
	puts("        :CFBundleDocumentTypes:2:CFBundleTypeExtensions");
	putchar('\n');
	puts("Types:");
	puts("    string");
	puts("    array");
	puts("    dict");
	puts("    bool");
	puts("    real");
	puts("    integer");
	puts("    date");
	puts("    data");
	putchar('\n');
	puts("Examples:");
	puts("    Set :CFBundleIdentifier com.apple.plistbuddy");
	puts("        Sets the CFBundleIdentifier property to com.apple.plistbuddy");
	puts("    Add :CFBundleGetInfoString string \"App version 1.0.1\"");
	puts("        Adds the CFBundleGetInfoString property to the plist");
	puts("    Add :CFBundleDocumentTypes: dict");
	puts("        Adds a new item of type dict to the CFBundleDocumentTypes array");
	puts("    Add :CFBundleDocumentTypes:0 dict");
	puts("        Adds the new item to the beginning of the array");
	puts("    Delete :CFBundleDocumentTypes:0 dict");
	puts("        Deletes the FIRST item in the array");
	puts("    Delete :CFBundleDocumentTypes");
	puts("        Deletes the ENTIRE CFBundleDocumentTypes array");
	putchar('\n');
}

static void
PrintUsage(char **argv)
{
	char *p = argv[0];
	char *e = p + strlen(p);
	char *name = e + 1;

	if (e > p) {
		char *q = e - 1;
		while (q > p && *q != '/')
			q--;
		name = q + 1;
	}
	printf("Usage: %s [-cxlh] <file.plist>\n", name);
	puts("    -c \"<command>\" execute command, otherwise run in interactive mode");
	puts("    -x output will be in the form of an xml plist where appropriate");
	puts("    -l if the path to <file.plist> contains symbolic links, they will\n"
	    "       not be followed.");
	puts("    -h print the complete help info, with command guide");
	putchar('\n');
}

static void
ExecuteCommand(State *s, Command *c)
{
	switch (c->cmd) {
	case kCmdHelp:
		PrintHelp();
		return;
	case kCmdExit:
		s->running = false;
		return;
	case kCmdSave:
		puts("Saving...");
		SavePlist(s->root, s->path, s->noFollow);
		s->dirty = false;
		return;
	case kCmdRevert:
		puts("Reverting to last saved state...");
		RevertPlist(s);
		s->dirty = false;
		return;
	case kCmdClear: {
		puts("Initializing Plist...");
		CFRelease(s->root);
		char empty = '\0';
		if (c->type == kTypeNone)
			c->type = kTypeDict;
		s->root = s->working = CreateEntryWithValue(c->type, 1, &empty);
		s->dirty = true;
		return;
	}
	case kCmdPrint:
		CommandPrint(s, c);
		return;
	case kCmdCD:
		CommandCD(s, c);
		return;
	case kCmdSet:
		CommandSet(s, c);
		break;
	case kCmdAdd:
		CommandAdd(s, c);
		break;
	case kCmdCopy:
		CommandCopy(s, c);
		break;
	case kCmdDelete:
		CommandDelete(s, c);
		break;
	case kCmdMerge:
		CommandMerge(s, c);
		break;
	case kCmdImport:
		CommandImport(s, c);
		break;
	default:
		return;
	}
	s->dirty = true;
}

/* ---------------------------------------------------------------- parser */

static int
PropertyTypeFromString(const char *s)
{
	static const char *const names[] = {
		"string", "array", "dict", "bool", "real", "integer", "date", "data"
	};

	for (int i = 0; i < 8; i++)
		if (strcasecmp(s, names[i]) == 0)
			return i + 1;
	return kTypeNone;
}

/*
 * Scan one atom starting at s[*pos] into out (NULL to only measure).
 * Backslash escapes \t, \n and any character; "\:" is kept as is so
 * GetEntryName can tell it from a separator.  Single and double quotes
 * group; with split set, an unquoted blank ends the atom.  Reaching the end
 * of the string stores and counts its NUL.  Returns the length, or 0 after
 * reporting an unclosed quote.
 */
static int
ParseAtom(const char *s, char *out, int *pos, int split)
{
	bool esc = false, sq = false, dq = false;
	int n = 0;

	for (;;) {
		char c = s[*pos];
		if (esc) {
			esc = false;
			if (c == ':') {
				if (out) {
					out[n] = '\\';
					out[n + 1] = ':';
				}
				n += 2;
				goto next;
			}
			if (out)
				out[n] = c == 't' ? '\t' : c == 'n' ? '\n' : c;
			n++;
		} else if (c == '\\') {
			esc = true;
		} else if (c == '"') {
			dq = !dq;
		} else if (c == '\'') {
			sq = !sq;
		} else if (split && !sq && !dq && (c == ' ' || c == '\t')) {
			break;
		} else {
			if (out)
				out[n] = c;
			n++;
		}
next:
		if (s[*pos] == '\0')
			break;
		(*pos)++;
	}
	if (sq || dq) {
		puts("Parse Error: Unclosed Quotes");
		return 0;
	}
	if (out)
		out[n] = '\0';
	return n;
}

static char *
ExtractAtom(const char *s, const char **rest, int split)
{
	while (*s == ' ' || *s == '\t')
		s++;
	int pos = 0;
	int n = ParseAtom(s, NULL, &pos, split);
	if (n < 1)
		return NULL;
	char *atom = malloc(n + 1);
	pos = 0;
	ParseAtom(s, atom, &pos, split);
	if (rest != NULL)
		*rest = s + pos;
	return atom;
}

static int
ExtractPropertyType(const char *s, const char **rest, const char *err)
{
	char *atom = ExtractAtom(s, rest, 1);

	if (atom == NULL) {
		if (err != NULL)
			printf("%s", err);
		return kTypeNone;
	}
	int t = PropertyTypeFromString(atom);
	if (t == kTypeNone)
		printf("Unrecognized Type: %s\n", atom);
	free(atom);
	return t;
}

static char *
ExtractEntry(const char *s, const char **rest, const char *err)
{
	char *e = ExtractAtom(s, rest, 1);

	if (err != NULL && e == NULL)
		printf("%s", err);
	return e;
}

static bool
VerifyCommandToken(const char *line, const char *token, const char **rest)
{
	size_t len = strlen(token);

	if (strncasecmp(line, token, len) != 0)
		return false;
	*rest = line + len;
	return true;
}

static bool
ParseCmnd(const char *line, Command *c)
{
	const char *rest = NULL;

#define TOKEN(t) VerifyCommandToken(line, t, &rest)
	if (strncasecmp(line, "Help", 4) == 0 || strncasecmp(line, "?", 1) == 0) {
		c->cmd = kCmdHelp;
		return true;
	}
	if (strncasecmp(line, "Exit", 4) == 0 ||
	    strncasecmp(line, "Quit", 4) == 0 || TOKEN("Bye")) {
		c->cmd = kCmdExit;
		return true;
	}
	if (TOKEN("Save")) {
		c->cmd = kCmdSave;
		return true;
	}
	if (TOKEN("Revert")) {
		c->cmd = kCmdRevert;
		return true;
	}
	if (TOKEN("Clear")) {
		c->cmd = kCmdClear;
		c->type = ExtractPropertyType(rest, &rest, NULL);
		return true;
	}
	if (TOKEN("Print") || TOKEN("ls")) {
		c->cmd = kCmdPrint;
		c->entry = ExtractAtom(rest, &rest, 1);
		return true;
	}
	if (TOKEN("Set") || TOKEN("=")) {
		c->cmd = kCmdSet;
		c->entry = ExtractEntry(rest, &rest,
		    "Entry Required for Set Command\n");
		if (c->entry == NULL)
			return false;
		c->value = ExtractAtom(rest, &rest, 0);
		if (c->value == NULL) {
			c->valueLen = 0;
			puts("Value Required for Set Command");
			return false;
		}
		c->valueLen = strlen(c->value);
		return true;
	}
	if (TOKEN("Add") || TOKEN("+")) {
		c->cmd = kCmdAdd;
		c->entry = ExtractEntry(rest, &rest,
		    "Entry Required for Add Command\n");
		if (c->entry == NULL)
			return false;
		c->type = ExtractPropertyType(rest, &rest,
		    "Type Required for Add Command\n");
		if (c->type == kTypeNone)
			return false;
		c->value = ExtractAtom(rest, NULL, 0);
		c->valueLen = c->value ? strlen(c->value) : 0;
		return true;
	}
	if (TOKEN("Copy") || TOKEN("cp")) {
		c->cmd = kCmdCopy;
		c->entry = ExtractEntry(rest, &rest,
		    "Entry Required for Copy Command\n");
		if (c->entry == NULL)
			return false;
		c->entry2 = ExtractEntry(rest, &rest,
		    "Two Entries Required for Copy Command\n");
		return c->entry2 != NULL;
	}
	if (TOKEN("Delete") || TOKEN("Remove") || TOKEN("rm") || TOKEN("-")) {
		c->cmd = kCmdDelete;
		c->entry = ExtractEntry(rest, &rest,
		    "Entry Required for Delete Command\n");
		return c->entry != NULL;
	}
	if (TOKEN("Merge")) {
		c->cmd = kCmdMerge;
		c->value = ExtractAtom(rest, &rest, 1);
		if (c->value != NULL && strcasecmp(c->value, "-r") == 0) {
			c->flags = "r";
			c->value = ExtractAtom(rest, &rest, 1);	/* leaks "-r", as Apple's */
		}
		if (c->value == NULL) {
			c->valueLen = 0;
			puts("File required for Merge Command");
			return false;
		}
		c->valueLen = strlen(c->value);
		c->entry = ExtractAtom(rest, NULL, 1);
		return true;
	}
	if (TOKEN("Import")) {
		c->cmd = kCmdImport;
		c->entry = ExtractEntry(rest, &rest,
		    "Entry Required for Import Command\n");
		if (c->entry == NULL)
			return false;
		c->value = ExtractAtom(rest, &rest, 1);
		if (c->value == NULL) {
			c->valueLen = 0;
			puts("File required for Import Command");
			return false;
		}
		c->valueLen = strlen(c->value);
		return true;
	}
	if (TOKEN("CD")) {
		c->cmd = kCmdCD;
		c->entry = ExtractEntry(rest, &rest,
		    "Entry Required for CD Command\n");
		return c->entry != NULL;
	}
#undef TOKEN
	puts("Unrecognized Command");
	return false;
}

/* Read one line (at most 511 bytes) from stdin; EOF means Exit. */
static int
ReadCommand(Command *c)
{
	char buf[512];
	long i = 0;

	printf("Command: ");
	do {
		if (scanf("%c", &buf[i]) == EOF) {
			c->cmd = kCmdExit;
			return 1;
		}
	} while (buf[i] != '\n' && ++i != 511);
	buf[i] = '\0';
	if (buf[0] == '\0' || !ParseCmnd(buf, c))
		return 0;
	return 1;
}

/* ------------------------------------------------------------------ main */

static int
ParseArgs(int argc, char **argv, State *s, Command **cmds)
{
	static const char *const rewrite[][2] = {
		{ "/tmp/", "/private/tmp/" },
		{ "/etc/", "/private/etc/" },
		{ "/var/", "/private/var/" },
	};
	uint32_t status = 0;
	int ncmds = 0;
	char buf[0x400];

	cmds[0] = NULL;
	if (csops(getpid(), CS_OPS_STATUS, &status, sizeof(status)) == 0 &&
	    (status & CS_INSTALLER)) {
		fputs("Running with CS_INSTALLER, symlinks will not be followed.\n",
		    stderr);
		s->noFollow = true;
	}

	for (int i = 1; i < argc; i++) {
		char *arg = argv[i];

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0 ||
		    strcmp(arg, "-help") == 0) {
			PrintHelp();
			return 0;
		}
		if (strcmp(arg, "-c") == 0) {
			cmds[ncmds] = CreateCommand();
			if (++i == argc)
				goto usage;
			if (!ParseCmnd(argv[i], cmds[ncmds])) {
				DisposeCommand(cmds[ncmds]);
				cmds[ncmds] = NULL;
				return 0;
			}
			cmds[++ncmds] = NULL;
			continue;
		}
		if (strcmp(arg, "-x") == 0) {
			s->xml = 1;
			continue;
		}
		if (strcmp(arg, "-l") == 0) {
			s->noFollow = true;
			continue;
		}

		/* Anything else is the plist path; only one is allowed. */
		if (s->path != NULL) {
			puts("Invalid Arguments\n");
			return 0;
		}
		size_t k;
		for (k = 0; k < 3; k++) {
			if (strcasestr(arg, rewrite[k][0]) != arg)
				continue;
			if ((unsigned)snprintf(buf, sizeof(buf), "%s%s",
			    rewrite[k][1], arg + strlen(rewrite[k][0])) >=
			    sizeof(buf)) {
				errno = EMSGSIZE;
				printf("Invalid input path: %s\n", arg);
				return 0;
			}
			break;
		}
		if (k == 3)
			strlcpy(buf, arg, sizeof(buf));
		size_t len = strlen(buf);
		s->path = malloc(len + 1);
		strlcpy(s->path, buf, len + 1);
		if (!RevertPlist(s))
			return 0;
	}
	if (s->root != NULL)
		return 1;
usage:
	PrintUsage(argv);
	return 0;
}

int
main(int argc, char **argv)
{
	State s = { .xml = 0, .running = true };
	Command **cmds = calloc(argc + 1, sizeof(*cmds));

	if (!ParseArgs(argc, argv, &s, cmds)) {
		for (Command **c = cmds; *c != NULL; c++)
			DisposeCommand(*c);
		returnCode = 1;
	} else if (cmds[0] == NULL) {
		while (s.running) {
			Command *c = CreateCommand();
			if (ReadCommand(c))
				ExecuteCommand(&s, c);
			DisposeCommand(c);
		}
	} else {
		for (Command **c = cmds; *c != NULL; c++) {
			ExecuteCommand(&s, *c);
			DisposeCommand(*c);
		}
		if (s.dirty)
			SavePlist(s.root, s.path, s.noFollow);
	}
	if (s.root != NULL)
		CFRelease(s.root);
	free(s.path);
	free(cmds);
	return returnCode;
}
