#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/manifest.h"
#include "forge_util.h"

#define FORGE_MANIFEST_LINE_MAX 4096U

typedef enum ForgeManifestSection {
    FORGE_SECTION_NONE,
    FORGE_SECTION_PROJECT,
    FORGE_SECTION_SOURCES,
    FORGE_SECTION_TARGETS,
    FORGE_SECTION_BUILD,
    FORGE_SECTION_DEPENDENCIES,
    FORGE_SECTION_OVERRIDES,
    FORGE_SECTION_PROFILE_DEBUG,
    FORGE_SECTION_PROFILE_RELEASE
} ForgeManifestSection;

/* One key = "value" pair inside an inline table ({ git = "...", tag = "..." }). */
#define FORGE_INLINE_KEY_MAX 32U
#define FORGE_INLINE_MAX_ENTRIES 20U

typedef struct ForgeInlineEntry {
    char key[FORGE_INLINE_KEY_MAX];
    char value[FORGE_MANIFEST_VALUE_MAX];
} ForgeInlineEntry;

typedef struct ForgeProfileSeen {
    int opt_level;
    int debug_info;
    int warnings_as_errors;
    int std_version;
    int cflags;
} ForgeProfileSeen;

typedef struct ForgeManifestSeen {
    int project_name;
    int project_version;
    int source_c;
    int source_cpp;
    int source_asm;
    int target_os;
    int target_arch;
    int compiler;
    ForgeProfileSeen debug;
    ForgeProfileSeen release;
} ForgeManifestSeen;

static void remove_comment(char *text)
{
    int quoted = 0;
    int escaped = 0;
    char *cursor;

    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (escaped) {
            escaped = 0;
        } else if (*cursor == '\\' && quoted) {
            escaped = 1;
        } else if (*cursor == '"') {
            quoted = !quoted;
        } else if (*cursor == '#' && !quoted) {
            *cursor = '\0';
            return;
        }
    }
}

static int parse_string(char **cursor, char *destination, size_t destination_size,
                        char *error, size_t error_size)
{
    char *source = *cursor;
    size_t length = 0U;

    if (*source != '"') {
        forge_util_set_error(error, error_size, "expected a quoted string");
        return -1;
    }

    ++source;
    while (*source != '\0' && *source != '"') {
        char character = *source++;

        if (character == '\\') {
            if (*source != '"' && *source != '\\') {
                forge_util_set_error(error, error_size,
                          "only \\\" and \\\\ escapes are supported");
                return -1;
            }
            character = *source++;
        }

        if (length + 1U >= destination_size) {
            forge_util_set_error(error, error_size, "string exceeds %u characters",
                      (unsigned int)(destination_size - 1U));
            return -1;
        }
        destination[length++] = character;
    }

    if (*source != '"') {
        forge_util_set_error(error, error_size, "unterminated quoted string");
        return -1;
    }

    destination[length] = '\0';
    *cursor = source + 1;
    return 0;
}

static int parse_string_list(char *value, ForgeStringList *list,
                             char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);

    if (*cursor != '[') {
        forge_util_set_error(error, error_size, "expected a string array starting with '['");
        return -1;
    }

    ++cursor;
    cursor = forge_util_trim(cursor);
    list->count = 0U;
    if (*cursor == ']') {
        cursor = forge_util_trim(cursor + 1);
        if (*cursor != '\0') {
            forge_util_set_error(error, error_size, "unexpected text after string array");
            return -1;
        }
        return 0;
    }

    for (;;) {
        if (list->count == FORGE_MANIFEST_MAX_ITEMS) {
            forge_util_set_error(error, error_size, "array has more than %u entries",
                      FORGE_MANIFEST_MAX_ITEMS);
            return -1;
        }
        if (parse_string(&cursor, list->items[list->count],
                         sizeof(list->items[list->count]), error, error_size) != 0) {
            return -1;
        }
        if (list->items[list->count][0] == '\0') {
            forge_util_set_error(error, error_size, "empty strings are not allowed in arrays");
            return -1;
        }
        ++list->count;

        cursor = forge_util_trim(cursor);
        if (*cursor == ']') {
            cursor = forge_util_trim(cursor + 1);
            if (*cursor != '\0') {
                forge_util_set_error(error, error_size, "unexpected text after string array");
                return -1;
            }
            return 0;
        }
        if (*cursor != ',') {
            forge_util_set_error(error, error_size, "expected ',' or ']' in string array");
            return -1;
        }
        cursor = forge_util_trim(cursor + 1);
        if (*cursor == ']') {
            forge_util_set_error(error, error_size, "trailing commas are not allowed in arrays");
            return -1;
        }
    }
}

static ForgeManifestSection parse_section(const char *text)
{
    if (strcmp(text, "[project]") == 0) {
        return FORGE_SECTION_PROJECT;
    }
    if (strcmp(text, "[sources]") == 0) {
        return FORGE_SECTION_SOURCES;
    }
    if (strcmp(text, "[targets]") == 0) {
        return FORGE_SECTION_TARGETS;
    }
    if (strcmp(text, "[build]") == 0) {
        return FORGE_SECTION_BUILD;
    }
    if (strcmp(text, "[dependencies]") == 0) {
        return FORGE_SECTION_DEPENDENCIES;
    }
    if (strcmp(text, "[overrides]") == 0) {
        return FORGE_SECTION_OVERRIDES;
    }
    if (strcmp(text, "[profile.debug]") == 0) {
        return FORGE_SECTION_PROFILE_DEBUG;
    }
    if (strcmp(text, "[profile.release]") == 0) {
        return FORGE_SECTION_PROFILE_RELEASE;
    }
    return FORGE_SECTION_NONE;
}

static int parse_scalar(char *value, char *destination, size_t destination_size,
                        char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);

    if (parse_string(&cursor, destination, destination_size, error, error_size) != 0) {
        return -1;
    }
    cursor = forge_util_trim(cursor);
    if (*cursor != '\0') {
        forge_util_set_error(error, error_size, "unexpected text after quoted string");
        return -1;
    }
    if (destination[0] == '\0') {
        forge_util_set_error(error, error_size, "empty strings are not allowed");
        return -1;
    }
    return 0;
}

static int reject_duplicate(int *seen, const char *key, char *error, size_t error_size)
{
    if (*seen) {
        forge_util_set_error(error, error_size, "duplicate field '%s'", key);
        return -1;
    }
    *seen = 1;
    return 0;
}

static int parse_integer(char *value, int minimum, int maximum, int *output,
                         char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);
    char *end = NULL;
    long parsed;

    if (*cursor == '\0') {
        forge_util_set_error(error, error_size, "expected an integer");
        return -1;
    }
    parsed = strtol(cursor, &end, 10);
    if (end == cursor || *forge_util_trim(end) != '\0' ||
        parsed < minimum || parsed > maximum) {
        forge_util_set_error(error, error_size,
                  "expected an integer between %d and %d", minimum, maximum);
        return -1;
    }
    *output = (int)parsed;
    return 0;
}

static int parse_boolean(char *value, int *output, char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);

    if (strcmp(cursor, "true") == 0) {
        *output = 1;
        return 0;
    }
    if (strcmp(cursor, "false") == 0) {
        *output = 0;
        return 0;
    }
    forge_util_set_error(error, error_size, "expected 'true' or 'false'");
    return -1;
}

/* Dependency names become include paths and link inputs, so restrict them to
 * the same portable ASCII charset forge uses everywhere else (explicit ranges
 * rather than isalnum, which is locale-dependent). */
static int dependency_name_is_valid(const char *name)
{
    size_t index;

    for (index = 0U; name[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)name[index];

        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_' || character == '.')) {
            return 0;
        }
    }
    return name[0] != '\0';
}

/*
 * Feature names travel in cache directory suffixes, lock pins, and identity
 * strings: letters, digits, '-' and '_' only (no dots, which would collide
 * with the [features.name] reading), 1-32 characters.
 */
int forge_feature_name_is_valid(const char *name)
{
    size_t length;
    size_t index;

    if (name == NULL) {
        return 0;
    }
    length = strlen(name);
    if (length == 0U || length > FORGE_FEATURE_NAME_MAX) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char character = (unsigned char)name[index];

        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_')) {
            return 0;
        }
    }
    return 1;
}

/*
 * Versions follow the semver core shape: three numeric components without
 * leading zeros, optionally followed by a '-' pre-release of alphanumeric,
 * hyphen, or dot characters ("1.2.3", "1.2.3-rc.1"). Strictness here means
 * the version can be embedded as a C string literal and later sorted or
 * compared mechanically without edge cases.
 */
static int version_is_valid(const char *version)
{
    size_t index = 0U;
    unsigned int component;

    for (component = 0U; component < 3U; ++component) {
        if (component > 0U) {
            if (version[index] != '.') {
                return 0;
            }
            ++index;
        }
        if (!isdigit((unsigned char)version[index])) {
            return 0;
        }
        /* "0" alone is valid; "01" is not. */
        if (version[index] == '0' && isdigit((unsigned char)version[index + 1U])) {
            return 0;
        }
        while (isdigit((unsigned char)version[index])) {
            ++index;
        }
    }
    if (version[index] == '\0') {
        return 1;
    }
    if (version[index] != '-') {
        return 0;
    }
    ++index;
    if (version[index] == '\0') {
        return 0;
    }
    for (; version[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)version[index];

        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '.')) {
            return 0;
        }
    }
    return 1;
}

/*
 * Total semver precedence for validated versions ("1.2.3", optional
 * "-prerelease"): numeric MAJOR/MINOR/PATCH first, a release outranks any
 * pre-release of the same core, and pre-release identifiers compare per
 * semver 11.4 (numeric identifiers numerically and below alphanumeric
 * ones, alphanumeric ones lexically, a longer set wins a shared prefix).
 * A "+build" suffix is ignored, per semver 10. Returns -1/0/1.
 * Inputs must pass version_is_valid (modulo a build suffix); anything else
 * compares by fallback so resolution never spins on corrupt data.
 */
static int identifier_is_numeric(const char *text, size_t length)
{
    size_t index;

    if (length == 0U) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        if (!isdigit((unsigned char)text[index])) {
            return 0;
        }
    }
    return 1;
}

/* Numeric identifiers compare by value: leading zeros skipped, then the
 * longer digit run wins, then lexicographic order decides. */
static int compare_numeric_identifier(const char *a, size_t length_a,
                                      const char *b, size_t length_b)
{
    int order;

    while (length_a > 1U && a[0] == '0') {
        ++a;
        --length_a;
    }
    while (length_b > 1U && b[0] == '0') {
        ++b;
        --length_b;
    }
    if (length_a != length_b) {
        return length_a < length_b ? -1 : 1;
    }
    order = memcmp(a, b, length_a);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

static int compare_prerelease(const char *a, const char *b)
{
    for (;;) {
        const char *dot_a;
        const char *dot_b;
        size_t length_a;
        size_t length_b;
        int numeric_a;
        int numeric_b;

        if (*a == '\0' && *b == '\0') {
            return 0;
        }
        if (*a == '\0') {
            return -1;
        }
        if (*b == '\0') {
            return 1;
        }
        dot_a = strchr(a, '.');
        dot_b = strchr(b, '.');
        length_a = dot_a != NULL ? (size_t)(dot_a - a) : strlen(a);
        length_b = dot_b != NULL ? (size_t)(dot_b - b) : strlen(b);
        numeric_a = identifier_is_numeric(a, length_a);
        numeric_b = identifier_is_numeric(b, length_b);
        if (numeric_a && numeric_b) {
            int order = compare_numeric_identifier(a, length_a, b, length_b);

            if (order != 0) {
                return order;
            }
        } else if (numeric_a) {
            return -1;
        } else if (numeric_b) {
            return 1;
        } else {
            size_t shortest = length_a < length_b ? length_a : length_b;
            int order = shortest == 0U ? 0 : memcmp(a, b, shortest);

            if (order != 0) {
                return order < 0 ? -1 : 1;
            }
            if (length_a != length_b) {
                return length_a < length_b ? -1 : 1;
            }
        }
        a += length_a + (dot_a != NULL ? 1U : 0U);
        b += length_b + (dot_b != NULL ? 1U : 0U);
    }
}

int forge_version_compare(const char *a, const char *b)
{
    unsigned long parts_a[3] = { 0U, 0U, 0U };
    unsigned long parts_b[3] = { 0U, 0U, 0U };
    const char *pre_a;
    const char *pre_b;
    char core_a[FORGE_MANIFEST_VALUE_MAX];
    char core_b[FORGE_MANIFEST_VALUE_MAX];
    size_t index;

    if (a == NULL || b == NULL) {
        return a == b ? 0 : (a == NULL ? -1 : 1);
    }
    /* A "+build" suffix never affects precedence (semver 10). */
    (void)snprintf(core_a, sizeof(core_a), "%s", a);
    (void)snprintf(core_b, sizeof(core_b), "%s", b);
    {
        char *build_a = strchr(core_a, '+');
        char *build_b = strchr(core_b, '+');

        if (build_a != NULL) {
            *build_a = '\0';
        }
        if (build_b != NULL) {
            *build_b = '\0';
        }
    }
    /* sscanf never fails on validated versions; the fallback keeps the
     * order total so resolution cannot spin on corrupt data. */
    if (sscanf(core_a, "%lu.%lu.%lu", &parts_a[0], &parts_a[1], &parts_a[2]) != 3 ||
        sscanf(core_b, "%lu.%lu.%lu", &parts_b[0], &parts_b[1], &parts_b[2]) != 3) {
        int order = strcmp(a, b);

        return order < 0 ? -1 : (order > 0 ? 1 : 0);
    }
    for (index = 0U; index < 3U; ++index) {
        if (parts_a[index] != parts_b[index]) {
            return parts_a[index] < parts_b[index] ? -1 : 1;
        }
    }
    pre_a = strchr(core_a, '-');
    pre_b = strchr(core_b, '-');
    if (pre_a == NULL && pre_b == NULL) {
        return 0;
    }
    if (pre_a == NULL) {
        return 1;
    }
    if (pre_b == NULL) {
        return -1;
    }
    return compare_prerelease(pre_a + 1U, pre_b + 1U);
}

/* ------------------------------------------------------------------ */
/* Version requirements: full ranges                                   */
/*                                                                     */
/* Grammar (no heap, bounded buffers):                                 */
/*   range    := or_group (OR or_group)                                */
/*   or_group := term ("," term)                                       */
/*   term     := [op] version-or-wildcard, op in <= >= < > = ^ ~        */
/*   version-or-wildcard := star or partial with x-star + optional      */
/*     -prerelease (wildcards only in numeric core, never with ^ ~ < > */
/*     except "=" and bare which normalize them). Bare "1.2.3" means   */
/*     "=1.2.3". Comma = AND, OR = OR, spaces are ignored.             */
/* ------------------------------------------------------------------ */

static const char *range_skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

/* Parses one numeric component which may be digits or x/X/star. Returns 1 on
 * success with value/wild set; empty component fails. */
static int range_parse_component(const char **cursor, unsigned long *value,
                                 int *wild)
{
    const char *p = *cursor;

    *wild = 0;
    if (*p == 'x' || *p == 'X' || *p == '*') {
        *wild = 1;
        *value = 0UL;
        *cursor = p + 1U;
        return 1;
    }
    if (!isdigit((unsigned char)*p)) {
        return 0;
    }
    *value = 0UL;
    while (isdigit((unsigned char)*p)) {
        *value = *value * 10UL + (unsigned long)(*p - '0');
        ++p;
    }
    *cursor = p;
    return 1;
}

/* Validates prerelease tail "[ -prerelease ][ +build ]" charset; empty ok. */
static int range_prerelease_tail_valid(const char *p)
{
    size_t n = 0U;

    if (*p == '\0') {
        return 1;
    }
    if (*p == '+') {
        ++p;
        if (*p == '\0') {
            return 0;
        }
        for (; *p != '\0'; ++p) {
            unsigned char c = (unsigned char)*p;

            if (!(isalnum(c) || c == '-' || c == '.' || c == '+')) {
                return 0;
            }
            if (++n > 128U) {
                return 0;
            }
        }
        return 1;
    }
    if (*p != '-') {
        return 0;
    }
    ++p;
    if (*p == '\0') {
        return 0;
    }
    for (; *p != '\0' && *p != '+'; ++p) {
        unsigned char c = (unsigned char)*p;

        if (!(isalnum(c) || c == '-' || c == '.')) {
            return 0;
        }
        if (++n > 128U) {
            return 0;
        }
    }
    if (*p == '+') {
        return range_prerelease_tail_valid(p);
    }
    return *p == '\0';
}

/*
 * Parses one version-or-wildcard at *cursor (no operator, no spaces).
 * Fills parts[3], wild[3], has_pre (whether a -prerelease/+build tail was
 * present) and a normalized exact string when there are no wildcards.
 * Advances *cursor past the token. Returns 1 on success.
 */
static int range_parse_version_token(const char **cursor,
                                     unsigned long parts[3], int wild[3],
                                     int *has_pre, char *normalized,
                                     size_t normalized_size)
{
    const char *p = *cursor;
    const char *tail;
    char tail_buf[160];
    size_t tail_len;
    int dots = 0;

    if (*p == '*' || *p == 'x' || *p == 'X') {
        /* Lone wildcard: consume exactly one char; trailing alnum fails. */
        char c = p[1];
        if (c != '\0' && c != ' ' && c != '\t' && c != ',' && c != '|' &&
            c != '<' && c != '>' && c != '=' && c != '^' && c != '~') {
            return 0;
        }
        parts[0] = parts[1] = parts[2] = 0UL;
        wild[0] = wild[1] = wild[2] = 1;
        *has_pre = 0;
        if (normalized != NULL) {
            (void)snprintf(normalized, normalized_size, "*");
        }
        *cursor = p + 1U;
        return 1;
    }
    for (dots = 0; dots < 3; ++dots) {
        wild[dots] = 0;
    }
    {
        int npresent = 0;

        for (dots = 0; dots < 3; ++dots) {
            if (!range_parse_component(&p, &parts[dots], &wild[dots])) {
                return 0;
            }
            ++npresent;
            if (dots < 2) {
                if (*p == '.') {
                    ++p;
                    /* "1." or "1.x." with nothing after dot fails below. */
                    if (*p == '\0') {
                        return 0;
                    }
                    continue;
                }
                break;
            }
        }
        /* Missing trailing components (e.g. "1" or "1.2") act as wildcards,
         * npm-style: "1.2" means "1.2.x". Full "1.2.3" sets no wildcards. */
        for (dots = npresent; dots < 3; ++dots) {
            wild[dots] = 1;
            parts[dots] = 0UL;
        }
    }
    /* Wildcards must be trailing: "1.x.3" is invalid (parsed as numeric
     * after wild). A leading "x.1" never reaches here (lone-x consumes a
     * single char, so "x.1" fails at the '.' delimiter check below). */
    /* Wildcards must be trailing: "1.x.3" or "x.1" are invalid. */
    {
        int seen_wild = 0;
        int filled = 0;
        /* Count how many components were actually present by re-walking:
         * simpler rule — once wild, all later must be wild/unset-treated
         * as wild. Missing trailing components count as wild for ^ ~. */
        (void)filled;
        for (dots = 0; dots < 3; ++dots) {
            if (wild[dots]) {
                seen_wild = 1;
            } else if (seen_wild) {
                return 0;
            }
        }
        /* Leading wildcard like "x.1" already failed since x consumed as
         * lone token above only when single char; "x.1" reaches here with
         * wild[0]=1 then numeric -> rejected by the loop above. */
        if (wild[0]) {
            return 0;
        }
    }
    tail = p;
    tail_len = 0U;
    if (*p == '-' || *p == '+') {
        const char *q = p;

        while (*q != '\0' && *q != ' ' && *q != '\t' && *q != ',' &&
               *q != '|') {
            ++q;
        }
        tail_len = (size_t)(q - p);
        if (tail_len == 0U || tail_len >= sizeof(tail_buf)) {
            return 0;
        }
        memcpy(tail_buf, p, tail_len);
        tail_buf[tail_len] = '\0';
        if (!range_prerelease_tail_valid(tail_buf)) {
            return 0;
        }
        /* Wildcards never combine with a prerelease tail. */
        if (wild[1] || wild[2]) {
            return 0;
        }
        p = q;
    } else {
        tail_buf[0] = '\0';
        /* Stop at delimiter; anything else (e.g. letters) is invalid. */
        if (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',' &&
            *p != '|' && *p != '<' && *p != '>' && *p != '=' &&
            *p != '^' && *p != '~') {
            return 0;
        }
    }
    *has_pre = tail_buf[0] != '\0';
    if (normalized != NULL) {
        if (wild[1] || wild[2]) {
            (void)snprintf(normalized, normalized_size, "%lu.%lu.%lu",
                            parts[0], parts[1], parts[2]);
        } else if (*has_pre) {
            (void)snprintf(normalized, normalized_size, "%lu.%lu.%lu%s",
                            parts[0], parts[1], parts[2], tail_buf);
        } else {
            (void)snprintf(normalized, normalized_size, "%lu.%lu.%lu",
                            parts[0], parts[1], parts[2]);
        }
        /* Exact tokens without wildcards must be strict semver so later
         * forge_version_compare ordering is meaningful. */
        if (!wild[1] && !wild[2] && !version_is_valid(normalized)) {
            return 0;
        }
    }
    (void)tail;
    (void)tail_len;
    *cursor = p;
    return 1;
}

static void range_format_version(char *out, size_t out_size,
                                 unsigned long major, unsigned long minor,
                                 unsigned long patch)
{
    (void)snprintf(out, out_size, "%lu.%lu.%lu", major, minor, patch);
}

/* Evaluates one comparator against validated `version`. `op` is one of
 * "", "=", "<=", ">=", "<", ">", "^", "~". Returns -1 on malformed
 * comparator (validate-only also fails), 0 = no match, 1 = match. */
static int range_test_comparator(const char *version, const char *op,
                                 unsigned long parts[3], int wild[3],
                                 const char *exact)
{
    char lower[FORGE_MANIFEST_VALUE_MAX];
    char upper[FORGE_MANIFEST_VALUE_MAX];

    /* Lone "*" matches everything (including prereleases of same core?
     * keep simple: matches any validated version). */
    if (wild[0] && wild[1] && wild[2]) {
        return 1;
    }
    if (strcmp(op, "^") == 0 || strcmp(op, "~") == 0) {
        /* Wildcards never combine with ^ ~ (validated earlier). */
        if (wild[1] || wild[2]) {
            return -1;
        }
        range_format_version(lower, sizeof(lower), parts[0], parts[1],
                             parts[2]);
        /* Preserve prerelease tail for the lower bound. */
        if (exact != NULL && strchr(exact, '-') != NULL) {
            (void)snprintf(lower, sizeof(lower), "%s", exact);
        }
        if (forge_version_compare(version, lower) < 0) {
            return 0;
        }
        if (strcmp(op, "^") == 0) {
            if (parts[0] != 0UL) {
                range_format_version(upper, sizeof(upper), parts[0] + 1U,
                                     0UL, 0UL);
            } else if (parts[1] != 0UL) {
                range_format_version(upper, sizeof(upper), 0UL,
                                     parts[1] + 1U, 0UL);
            } else {
                /* ^0.0.z is exact. */
                return forge_version_compare(version, lower) == 0 ? 1 : 0;
            }
        } else {
            /* ~1.2.3 := >=1.2.3 <1.3.0; ~1.2 / ~1 use first two/one. */
            range_format_version(upper, sizeof(upper), parts[0],
                                 parts[1] + 1U, 0UL);
        }
        return forge_version_compare(version, upper) < 0 ? 1 : 0;
    }
    if (wild[1] || wild[2]) {
        /* Wildcard terms only valid as bare/= (validated earlier). */
        if (strcmp(op, "") != 0 && strcmp(op, "=") != 0 &&
            strcmp(op, ">=") != 0) {
            return -1;
        }
        if (wild[1] && !wild[0]) {
            /* "1.*" / "1.x": >=1.0.0 <2.0.0 */
            range_format_version(lower, sizeof(lower), parts[0], 0UL, 0UL);
            range_format_version(upper, sizeof(upper), parts[0] + 1U, 0UL,
                                 0UL);
        } else {
            /* "1.2.*" : >=1.2.0 <1.3.0 */
            range_format_version(lower, sizeof(lower), parts[0], parts[1],
                                 0UL);
            range_format_version(upper, sizeof(upper), parts[0],
                                 parts[1] + 1U, 0UL);
        }
        if (strcmp(op, ">=") == 0) {
            return forge_version_compare(version, lower) >= 0 ? 1 : 0;
        }
        if (forge_version_compare(version, lower) < 0) {
            return 0;
        }
        return forge_version_compare(version, upper) < 0 ? 1 : 0;
    }
    /* Exact numeric reference. */
    {
        char ref[FORGE_MANIFEST_VALUE_MAX];

        if (exact != NULL && exact[0] != '\0') {
            (void)snprintf(ref, sizeof(ref), "%s", exact);
        } else {
            range_format_version(ref, sizeof(ref), parts[0], parts[1],
                                 parts[2]);
        }
        if (strcmp(op, "") == 0 || strcmp(op, "=") == 0) {
            return forge_version_compare(version, ref) == 0 ? 1 : 0;
        }
        if (strcmp(op, ">=") == 0) {
            return forge_version_compare(version, ref) >= 0 ? 1 : 0;
        }
        if (strcmp(op, ">") == 0) {
            return forge_version_compare(version, ref) > 0 ? 1 : 0;
        }
        if (strcmp(op, "<=") == 0) {
            return forge_version_compare(version, ref) <= 0 ? 1 : 0;
        }
        if (strcmp(op, "<") == 0) {
            return forge_version_compare(version, ref) < 0 ? 1 : 0;
        }
        return -1;
    }
}

/* Core range engine: version==NULL means validate-only. Returns 1 when the
 * range is well-formed (and, when version != NULL, satisfied). */
static int range_eval(const char *version, const char *range)
{
    const char *p;

    if (range == NULL || range[0] == '\0') {
        return 0;
    }
    if (strlen(range) >= FORGE_VERSION_RANGE_MAX) {
        return 0;
    }
    if (version != NULL && !version_is_valid(version)) {
        return 0;
    }
    p = range;
    for (;;) {
        int or_ok = 1; /* AND group result */
        int group_has_term = 0;

        for (;;) {
            char op[3] = {0};
            unsigned long parts[3] = {0U, 0U, 0U};
            int wild[3] = {0, 0, 0};
            int has_pre = 0;
            char exact[FORGE_MANIFEST_VALUE_MAX] = {0};
            int r;

            p = range_skip_spaces(p);
            if (*p == '\0') {
                if (!group_has_term) {
                    return 0; /* trailing || or empty */
                }
                break;
            }
            if (*p == ',') {
                return 0; /* leading / double comma */
            }
            if (p[0] == '|' && p[1] == '|') {
                break; /* end of AND group */
            }
            /* Operator prefix. */
            if (p[0] == '<' && p[1] == '=') {
                op[0] = '<'; op[1] = '=';
                p += 2;
            } else if (p[0] == '>' && p[1] == '=') {
                op[0] = '>'; op[1] = '=';
                p += 2;
            } else if (*p == '<' || *p == '>' || *p == '=' ||
                       *p == '^' || *p == '~') {
                op[0] = *p;
                p += 1;
            }
            p = range_skip_spaces(p);
            if (!range_parse_version_token(&p, parts, wild, &has_pre,
                                           exact, sizeof(exact))) {
                return 0;
            }
            /* Wildcards only with bare/= (and >= as floor sugar). */
            if ((wild[1] || wild[2]) &&
                !(strcmp(op, "") == 0 || strcmp(op, "=") == 0 ||
                  strcmp(op, ">=") == 0)) {
                return 0;
            }
            /* ^ ~ never take prerelease/wildcard tails (checked above). */
            if ((strcmp(op, "^") == 0 || strcmp(op, "~") == 0) &&
                (wild[1] || wild[2])) {
                return 0;
            }
            group_has_term = 1;
            if (version != NULL) {
                r = range_test_comparator(version, op, parts, wild, exact);
                if (r < 0) {
                    return 0;
                }
                if (!r) {
                    or_ok = 0;
                }
            }
            p = range_skip_spaces(p);
            if (*p == ',') {
                ++p;
                p = range_skip_spaces(p);
                if (*p == '\0' || (p[0] == '|' && p[1] == '|')) {
                    return 0; /* trailing comma */
                }
                continue;
            }
            if (p[0] == '|' && p[1] == '|') {
                break;
            }
            if (*p == '\0') {
                break;
            }
            /* Anything else (e.g. stray ops) ends the term invalidly. */
            if (*p == '<' || *p == '>' || *p == '=' || *p == '^' ||
                *p == '~') {
                /* Missing comma between comparators: ">=1.0.0 <2.0.0"
                 * space-separated form — accept as AND like npm. */
                continue;
            }
            return 0;
        }
        if (version != NULL && or_ok && group_has_term) {
            return 1;
        }
        p = range_skip_spaces(p);
        if (*p == '\0') {
            return version == NULL ? 1 : 0;
        }
        if (p[0] == '|' && p[1] == '|') {
            p += 2;
            p = range_skip_spaces(p);
            if (*p == '\0') {
                return 0;
            }
            continue;
        }
        return 0;
    }
}

int forge_version_range_is_valid(const char *range)
{
    size_t i;

    if (range == NULL || range[0] == '\0') {
        return 0;
    }
    /* Tight charset: digits, semver punctuation, range operators,
     * wildcards, spaces, prerelease/build tails. Anything else (shell
     * metachars, quotes, backslashes) fails closed. */
    for (i = 0U; range[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)range[i];

        if (!(isalnum(c) || c == '.' || c == '-' || c == '+' ||
              c == '<' || c == '>' || c == '=' || c == '^' ||
              c == '~' || c == ',' || c == '|' || c == '*' ||
              c == ' ' || c == '\t')) {
            return 0;
        }
    }
    return range_eval(NULL, range);
}

int forge_version_satisfies(const char *version, const char *range)
{
    if (range == NULL || range[0] == '\0') {
        return 1; /* no constraint satisfies everything */
    }
    if (version == NULL || version[0] == '\0') {
        return 0;
    }
    return range_eval(version, range);
}

/*
 * Parses a strict inline table: '{' key = "string" (, key = "string")* '}'.
 * Bare keys are short identifiers; values are quoted strings; trailing
 * commas are rejected like everywhere else in the manifest.
 */
static int parse_inline_table(char *value, ForgeInlineEntry *entries,
                              size_t max_entries, size_t *entry_count,
                              char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);
    size_t count = 0U;

    *entry_count = 0U;
    if (*cursor != '{') {
        forge_util_set_error(error, error_size,
                  "expected an inline table starting with '{'");
        return -1;
    }
    ++cursor;
    cursor = forge_util_trim(cursor);
    if (*cursor == '}') {
        cursor = forge_util_trim(cursor + 1);
        if (*cursor != '\0') {
            forge_util_set_error(error, error_size, "unexpected text after inline table");
            return -1;
        }
        return 0;
    }
    for (;;) {
        ForgeInlineEntry *entry;
        char *key_start;

        if (count == max_entries) {
            forge_util_set_error(error, error_size, "inline table has too many entries");
            return -1;
        }
        entry = &entries[count];
        key_start = cursor;
        while (isalnum((unsigned char)*cursor) || *cursor == '-' || *cursor == '_' ||
               *cursor == '.') {
            ++cursor;
        }
        if (cursor == key_start) {
            forge_util_set_error(error, error_size, "expected a key in inline table");
            return -1;
        }
        if ((size_t)(cursor - key_start) >= FORGE_INLINE_KEY_MAX) {
            forge_util_set_error(error, error_size, "inline table key is too long");
            return -1;
        }
        memcpy(entry->key, key_start, (size_t)(cursor - key_start));
        entry->key[cursor - key_start] = '\0';
        cursor = forge_util_trim(cursor);
        if (*cursor != '=') {
            forge_util_set_error(error, error_size,
                      "expected '=' after '%s' in inline table", entry->key);
            return -1;
        }
        cursor = forge_util_trim(cursor + 1);
        if (parse_string(&cursor, entry->value, sizeof(entry->value),
                         error, error_size) != 0) {
            return -1;
        }
        ++count;
        cursor = forge_util_trim(cursor);
        if (*cursor == '}') {
            cursor = forge_util_trim(cursor + 1);
            if (*cursor != '\0') {
                forge_util_set_error(error, error_size, "unexpected text after inline table");
                return -1;
            }
            *entry_count = count;
            return 0;
        }
        if (*cursor != ',') {
            forge_util_set_error(error, error_size,
                      "expected ',' or '}' in inline table");
            return -1;
        }
        cursor = forge_util_trim(cursor + 1);
        if (*cursor == '}') {
            forge_util_set_error(error, error_size,
                      "trailing commas are not allowed in inline tables");
            return -1;
        }
    }
}

static const ForgeInlineEntry *find_inline_entry(const ForgeInlineEntry *entries,
                                                 size_t count, const char *key)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (strcmp(entries[index].key, key) == 0) {
            return &entries[index];
        }
    }
    return NULL;
}

/*
 * Splits a comma-separated feature list ("ssl, http") into dependency
 * slots. Inline tables only carry quoted strings, so features spell as one
 * string rather than an array; every name is validated here so typos fail
 * at parse time (existence is checked against the recipe after fetch).
 * The stored list is sorted and deduplicated so cache directories, lock
 * pins, and identity strings are stable however the manifest spells it.
 */
int forge_parse_feature_list(const char *name, const char *text,
                             ForgeDependency *dependency,
                             char *error, size_t error_size)
{
    const char *cursor = text;
    size_t left;
    size_t slot;

    for (;;) {
        const char *item;
        size_t length;
        char candidate[FORGE_FEATURE_NAME_MAX + 1U];

        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        item = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            ++cursor;
        }
        length = (size_t)(cursor - item);
        while (length != 0U &&
               (item[length - 1U] == ' ' || item[length - 1U] == '\t')) {
            --length;
        }
        if (length > FORGE_FEATURE_NAME_MAX) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': bad feature name; use 1-%u of "
                      "letters, digits, '-', '_'",
                      name, (unsigned int)FORGE_FEATURE_NAME_MAX);
            return -1;
        }
        memcpy(candidate, item, length);
        candidate[length] = '\0';
        if (!forge_feature_name_is_valid(candidate)) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': bad feature name '%s'; use letters, "
                      "digits, '-', '_'",
                      name, candidate);
            return -1;
        }
        if (dependency->feature_count == FORGE_DEP_FEATURES_MAX) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': more than %u features",
                      name, (unsigned int)FORGE_DEP_FEATURES_MAX);
            return -1;
        }
        memcpy(dependency->features[dependency->feature_count], item, length);
        dependency->features[dependency->feature_count][length] = '\0';
        ++dependency->feature_count;
        if (*cursor == '\0') {
            break;
        }
        ++cursor; /* skip ',' */
    }
    /* Insertion sort plus dedupe into canonical order (memmove: the
     * slots overlap by construction, which snprintf forbids). */
    for (slot = 1U; slot < dependency->feature_count; ++slot) {
        char held[FORGE_FEATURE_NAME_MAX + 1U];
        size_t held_at = slot;

        memcpy(held, dependency->features[slot], sizeof(held));
        while (held_at > 0U &&
               strcmp(dependency->features[held_at - 1U], held) > 0) {
            memmove(dependency->features[held_at],
                    dependency->features[held_at - 1U],
                    sizeof(dependency->features[held_at]));
            --held_at;
        }
        memcpy(dependency->features[held_at], held, sizeof(held));
    }
    left = 0U;
    for (slot = 0U; slot < dependency->feature_count; ++slot) {
        if (left != 0U &&
            strcmp(dependency->features[slot],
                   dependency->features[left - 1U]) == 0) {
            continue;
        }
        if (slot != left) {
            memmove(dependency->features[left],
                    dependency->features[slot],
                    sizeof(dependency->features[left]));
        }
        ++left;
    }
    dependency->feature_count = left;
    return 0;
}

/* Build args travel as one comma-separated inline-table string (inline
 * tables only carry quoted strings, like features) and become argv
 * elements — never shell text. Each element must be non-empty, fit, and
 * avoid shell metacharacters so a manifest cannot smuggle `$(...)`,
 * backticks, redirects, or chaining into the CMake/Make invocation. */
static int build_arg_is_valid(const char *arg)
{
    size_t i;

    if (arg == NULL || arg[0] == '\0' ||
        strlen(arg) >= FORGE_MANIFEST_VALUE_MAX) {
        return 0;
    }
    for (i = 0U; arg[i] != '\0'; ++i) {
        char c = arg[i];

        if (c == ';' || c == '|' || c == '&' || c == '$' || c == '`' ||
            c == '\\' || c == '"' || c == '\'' || c == '(' ||
            c == ')' || c == '<' || c == '>' || c == '\n' ||
            c == '\r') {
            return 0;
        }
    }
    return 1;
}

static int parse_build_arg_list(const char *name, const char *field,
                                const char *text, char out[][FORGE_MANIFEST_VALUE_MAX],
                                size_t *count, char *error, size_t error_size)
{
    const char *cursor = text;

    *count = 0U;
    for (;;) {
        const char *item;
        size_t length;
        char candidate[FORGE_MANIFEST_VALUE_MAX];

        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        item = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            ++cursor;
        }
        length = (size_t)(cursor - item);
        while (length != 0U &&
               (item[length - 1U] == ' ' || item[length - 1U] == '\t')) {
            --length;
        }
        if (length == 0U || length >= FORGE_MANIFEST_VALUE_MAX) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': '%s' has an empty or overlong entry",
                      name, field);
            return -1;
        }
        memcpy(candidate, item, length);
        candidate[length] = '\0';
        if (!build_arg_is_valid(candidate)) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': '%s' entry '%s' uses shell "
                      "metacharacters; pass plain VAR=value or -D flags",
                      name, field, candidate);
            return -1;
        }
        if (*count == FORGE_BUILD_ARGS_MAX) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': '%s' lists more than %u entries",
                      name, field, (unsigned int)FORGE_BUILD_ARGS_MAX);
            return -1;
        }
        memcpy(out[*count], candidate, length + 1U);
        ++*count;
        if (*cursor == '\0') {
            break;
        }
        ++cursor; /* skip ',' */
    }
    return 0;
}

/* Toolchain paths become one -DCMAKE_TOOLCHAIN_FILE=<path> argv element
 * resolved against the dep root (or absolute). Only path-safe characters;
 * ".." is rejected here so the later within-root check cannot be dodged
 * by spelling. */
static int toolchain_path_is_valid(const char *path)
{
    size_t i;

    if (path == NULL || path[0] == '\0' ||
        strlen(path) >= FORGE_MANIFEST_VALUE_MAX) {
        return 0;
    }
    if (strstr(path, "..") != NULL) {
        return 0;
    }
    for (i = 0U; path[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)path[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == '/' || c == '\\' || c == ':' || c == ' ')) {
            return 0;
        }
    }
    return 1;
}

static int make_target_is_valid(const char *target)
{
    size_t i;

    if (target == NULL || target[0] == '\0' ||
        strlen(target) >= FORGE_MANIFEST_VALUE_MAX) {
        return 0;
    }
    for (i = 0U; target[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)target[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == '+')) {
            return 0;
        }
    }
    return 1;
}

static int parse_dependency_assignment(ForgeDependencyList *list, const char *name,
                                       char *value, char *error, size_t error_size)
{
    ForgeInlineEntry entries[FORGE_INLINE_MAX_ENTRIES];
    ForgeDependency *dependency;
    const ForgeInlineEntry *entry;
    const ForgeInlineEntry *git_entry;
    size_t count = 0U;
    size_t index;

    if (!dependency_name_is_valid(name)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid dependency name; use letters, digits, '-', '_', '.'",
                  name);
        return -1;
    }
    if (strlen(name) >= FORGE_MANIFEST_VALUE_MAX) {
        forge_util_set_error(error, error_size,
                  "dependency name is too long (%u characters maximum)",
                  (unsigned int)(FORGE_MANIFEST_VALUE_MAX - 1U));
        return -1;
    }
    for (index = 0U; index < list->count; ++index) {
        if (strcmp(list->items[index].name, name) == 0) {
            forge_util_set_error(error, error_size, "duplicate dependency '%s'", name);
            return -1;
        }
    }
    if (parse_inline_table(value, entries, FORGE_INLINE_MAX_ENTRIES, &count,
                           error, error_size) != 0) {
        return -1;
    }
    if (count == 0U) {
        forge_util_set_error(error, error_size,
                  "dependency '%s' needs a 'path', 'git', or 'registry' source", name);
        return -1;
    }
    entry = find_inline_entry(entries, count, "path");
    git_entry = find_inline_entry(entries, count, "git");
    {
        const ForgeInlineEntry *registry_entry =
            find_inline_entry(entries, count, "registry");
        const ForgeInlineEntry *version_entry =
            find_inline_entry(entries, count, "version");
        const ForgeInlineEntry *min_version_entry =
            find_inline_entry(entries, count, "min-version");
        const ForgeInlineEntry *range_entry =
            find_inline_entry(entries, count, "version-range");
        const ForgeInlineEntry *max_version_entry =
            find_inline_entry(entries, count, "max-version");
        const ForgeInlineEntry *features_entry =
            find_inline_entry(entries, count, "features");
        const ForgeInlineEntry *default_features_entry =
            find_inline_entry(entries, count, "default-features");
        int source_count = (entry != NULL) + (git_entry != NULL) +
                           (registry_entry != NULL);
        int pin_count = (version_entry != NULL) +
                        (min_version_entry != NULL) +
                        (range_entry != NULL);

        if (source_count != 1) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' needs exactly one source: 'path', "
                      "'git', or 'registry'", name);
            return -1;
        }
        if (version_entry != NULL && registry_entry == NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': version only applies to registry "
                      "dependencies", name);
            return -1;
        }
        if (min_version_entry != NULL && registry_entry == NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': min-version only applies to registry "
                      "dependencies", name);
            return -1;
        }
        if (range_entry != NULL && registry_entry == NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': version-range only applies to registry "
                      "dependencies", name);
            return -1;
        }
        if (max_version_entry != NULL && registry_entry == NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': max-version only applies to registry "
                      "dependencies", name);
            return -1;
        }
        if (pin_count > 1) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': use only one of version (exact pin), "
                      "min-version (minimum), or version-range (requirement)",
                      name);
            return -1;
        }
        if (max_version_entry != NULL && version_entry != NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': max-version cannot combine with an "
                      "exact version pin", name);
            return -1;
        }
        if ((features_entry != NULL || default_features_entry != NULL) &&
            registry_entry == NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': features only apply to registry "
                      "dependencies", name);
            return -1;
        }
        if (registry_entry != NULL) {
            if (find_inline_entry(entries, count, "tag") != NULL ||
                find_inline_entry(entries, count, "branch") != NULL ||
                find_inline_entry(entries, count, "rev") != NULL) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': refs only apply to git dependencies",
                          name);
                return -1;
            }
            if (find_inline_entry(entries, count, "submodules") != NULL) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': submodules only apply to git "
                          "dependencies", name);
                return -1;
            }
        }
    }
    if (find_inline_entry(entries, count, "submodules") != NULL && entry != NULL) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': submodules only apply to git dependencies",
                  name);
        return -1;
    }
    if (list->count == FORGE_MANIFEST_MAX_DEPS) {
        forge_util_set_error(error, error_size, "more than %u dependencies",
                  FORGE_MANIFEST_MAX_DEPS);
        return -1;
    }
    dependency = &list->items[list->count];
    (void)snprintf(dependency->name, sizeof(dependency->name), "%s", name);
    dependency->git_url[0] = '\0';
    dependency->ref[0] = '\0';
    dependency->path[0] = '\0';
    dependency->registry[0] = '\0';
    dependency->registry_version[0] = '\0';
    dependency->registry_min_version[0] = '\0';
    dependency->registry_range[0] = '\0';
    dependency->registry_max_version[0] = '\0';
    dependency->cmake_arg_count = 0U;
    dependency->cmake_toolchain[0] = '\0';
    dependency->make_arg_count = 0U;
    dependency->make_target[0] = '\0';
    dependency->feature_count = 0U;
    dependency->default_features = 1;
    if (entry != NULL) {
        if (find_inline_entry(entries, count, "tag") != NULL ||
            find_inline_entry(entries, count, "branch") != NULL ||
            find_inline_entry(entries, count, "rev") != NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': refs only apply to git dependencies", name);
            return -1;
        }
        if (find_inline_entry(entries, count, "version") != NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': version only applies to registry "
                      "dependencies", name);
            return -1;
        }
        (void)snprintf(dependency->path, sizeof(dependency->path), "%s", entry->value);
    } else if (git_entry != NULL) {
        static const char *const ref_keys[] = { "tag", "branch", "rev" };
        const ForgeInlineEntry *submodules_entry;
        size_t key_index;

        (void)snprintf(dependency->git_url, sizeof(dependency->git_url), "%s",
                       git_entry->value);
        submodules_entry = find_inline_entry(entries, count, "submodules");
        if (submodules_entry != NULL) {
            char flag_value[FORGE_MANIFEST_VALUE_MAX];

            /* parse_boolean trims in place, so it needs a writable copy. */
            (void)snprintf(flag_value, sizeof(flag_value), "%s",
                           submodules_entry->value);
            if (parse_boolean(flag_value, &dependency->submodules,
                              error, error_size) != 0) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': submodules must be \"true\" or \"false\"",
                          name);
                return -1;
            }
        }
        for (key_index = 0U; key_index < 3U; ++key_index) {
            entry = find_inline_entry(entries, count, ref_keys[key_index]);
            if (entry == NULL) {
                continue;
            }
            if (dependency->ref[0] != '\0') {
                forge_util_set_error(error, error_size,
                          "dependency '%s' has multiple refs; use one of tag/branch/rev",
                          name);
                return -1;
            }
            (void)snprintf(dependency->ref, sizeof(dependency->ref), "%s", entry->value);
        }
    } else {
        /* The source-count check above guarantees a registry entry here. */
        const ForgeInlineEntry *registry_entry =
            find_inline_entry(entries, count, "registry");
        const ForgeInlineEntry *version_entry =
            find_inline_entry(entries, count, "version");

        (void)snprintf(dependency->registry, sizeof(dependency->registry),
                       "%s", registry_entry->value);
        if (version_entry != NULL) {
            (void)snprintf(dependency->registry_version,
                           sizeof(dependency->registry_version), "%s",
                           version_entry->value);
        }
        {
            const ForgeInlineEntry *min_entry =
                find_inline_entry(entries, count, "min-version");
            const ForgeInlineEntry *range_entry =
                find_inline_entry(entries, count, "version-range");
            const ForgeInlineEntry *max_entry =
                find_inline_entry(entries, count, "max-version");

            if (min_entry != NULL) {
                if (!version_is_valid(min_entry->value)) {
                    forge_util_set_error(error, error_size,
                              "dependency '%s': min-version '%s' is not a "
                              "valid version; use MAJOR.MINOR.PATCH",
                              name, min_entry->value);
                    return -1;
                }
                (void)snprintf(dependency->registry_min_version,
                               sizeof(dependency->registry_min_version), "%s",
                               min_entry->value);
            }
            if (range_entry != NULL) {
                if (strlen(range_entry->value) >=
                    sizeof(dependency->registry_range)) {
                    forge_util_set_error(error, error_size,
                              "dependency '%s': version-range is too long "
                              "(max %u characters)", name,
                              (unsigned int)(sizeof(dependency->registry_range) - 1U));
                    return -1;
                }
                if (!forge_version_range_is_valid(range_entry->value)) {
                    forge_util_set_error(error, error_size,
                              "dependency '%s': version-range '%s' is not "
                              "valid; use comparators =, >=, >, <=, <, ^, ~, "
                              "wildcards x/*, ',' for AND, '||' for OR",
                              name, range_entry->value);
                    return -1;
                }
                (void)snprintf(dependency->registry_range,
                               sizeof(dependency->registry_range), "%s",
                               range_entry->value);
            }
            if (max_entry != NULL) {
                if (!version_is_valid(max_entry->value)) {
                    forge_util_set_error(error, error_size,
                              "dependency '%s': max-version '%s' is not a "
                              "valid version; use MAJOR.MINOR.PATCH",
                              name, max_entry->value);
                    return -1;
                }
                (void)snprintf(dependency->registry_max_version,
                               sizeof(dependency->registry_max_version), "%s",
                               max_entry->value);
            }
            if (min_entry != NULL && max_entry != NULL &&
                forge_version_compare(min_entry->value,
                                      max_entry->value) > 0) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': min-version '%s' exceeds "
                          "max-version '%s'", name, min_entry->value,
                          max_entry->value);
                return -1;
            }
        }
        {
            const ForgeInlineEntry *features_entry =
                find_inline_entry(entries, count, "features");
            const ForgeInlineEntry *defaults_entry =
                find_inline_entry(entries, count, "default-features");

            if (features_entry != NULL &&
                forge_parse_feature_list(name, features_entry->value, dependency,
                                   error, error_size) != 0) {
                return -1;
            }
            if (defaults_entry != NULL) {
                char flag_value[FORGE_MANIFEST_VALUE_MAX];

                /* parse_boolean trims in place, so it needs a writable copy. */
                (void)snprintf(flag_value, sizeof(flag_value), "%s",
                               defaults_entry->value);
                if (parse_boolean(flag_value, &dependency->default_features,
                                   error, error_size) != 0) {
                    forge_util_set_error(error, error_size,
                              "dependency '%s': default-features must be "
                              "\"true\" or \"false\"",
                              name);
                    return -1;
                }
            }
        }
    }
    /* Foreign-build tuning applies to every source type (path, git,
     * registry): it is consumed when the checkout builds foreign
     * (CMake/Make) and ignored for native Forge.toml checkouts. */
    {
        const ForgeInlineEntry *cmake_args_entry =
            find_inline_entry(entries, count, "cmake-args");
            const ForgeInlineEntry *toolchain_entry =
                find_inline_entry(entries, count, "cmake-toolchain");
            const ForgeInlineEntry *make_args_entry =
                find_inline_entry(entries, count, "make-args");
            const ForgeInlineEntry *make_target_entry =
                find_inline_entry(entries, count, "make-target");

            if (cmake_args_entry != NULL &&
                parse_build_arg_list(name, "cmake-args",
                                     cmake_args_entry->value,
                                     dependency->cmake_args,
                                     &dependency->cmake_arg_count,
                                     error, error_size) != 0) {
                return -1;
            }
            if (toolchain_entry != NULL) {
                if (!toolchain_path_is_valid(toolchain_entry->value)) {
                    forge_util_set_error(error, error_size,
                              "dependency '%s': cmake-toolchain '%s' is not "
                              "a safe path; use a relative or absolute file "
                              "path without '..' or shell characters",
                              name, toolchain_entry->value);
                    return -1;
                }
                (void)snprintf(dependency->cmake_toolchain,
                               sizeof(dependency->cmake_toolchain), "%s",
                               toolchain_entry->value);
            }
            if (make_args_entry != NULL &&
                parse_build_arg_list(name, "make-args",
                                     make_args_entry->value,
                                     dependency->make_args,
                                     &dependency->make_arg_count,
                                     error, error_size) != 0) {
                return -1;
            }
            if (make_target_entry != NULL) {
                if (!make_target_is_valid(make_target_entry->value)) {
                    forge_util_set_error(error, error_size,
                              "dependency '%s': make-target '%s' is not a "
                              "valid goal; use letters, digits, '-', '_', "
                              "'.', '+'",
                              name, make_target_entry->value);
                    return -1;
                }
                (void)snprintf(dependency->make_target,
                               sizeof(dependency->make_target), "%s",
                               make_target_entry->value);
            }
    }
    /* Reject unknown keys so typos fail loudly. */
    for (index = 0U; index < count; ++index) {
        static const char *const allowed[] = {
            "path", "git", "tag", "branch", "rev", "submodules",
            "registry", "version", "min-version", "version-range",
            "max-version", "features", "default-features",
            "cmake-args", "cmake-toolchain", "make-args", "make-target"
        };
        size_t allowed_index;
        int known = 0;

        for (allowed_index = 0U; allowed_index < 17U; ++allowed_index) {
            if (strcmp(entries[index].key, allowed[allowed_index]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            forge_util_set_error(error, error_size,
                      "unknown field '%s' in dependency '%s'", entries[index].key, name);
            return -1;
        }
    }
    ++list->count;
    return 0;
}

/* Applies a profile section assignment, sharing the parser between
 * [profile.debug] and [profile.release]. */
static int parse_profile_assignment(ForgeProfileSeen *seen, ForgeBuildProfile *profile,
                                    const char *key, char *value,
                                    char *error, size_t error_size)
{
    if (strcmp(key, "opt-level") == 0) {
        return reject_duplicate(&seen->opt_level, key, error, error_size) ||
               parse_integer(value, 0, 3, &profile->opt_level, error, error_size);
    }
    if (strcmp(key, "debug") == 0) {
        return reject_duplicate(&seen->debug_info, key, error, error_size) ||
               parse_boolean(value, &profile->debug_info, error, error_size);
    }
    if (strcmp(key, "warnings-as-errors") == 0) {
        return reject_duplicate(&seen->warnings_as_errors, key, error, error_size) ||
               parse_boolean(value, &profile->warnings_as_errors, error, error_size);
    }
    if (strcmp(key, "std") == 0) {
        return reject_duplicate(&seen->std_version, key, error, error_size) ||
               parse_scalar(value, profile->std_version, sizeof(profile->std_version),
                            error, error_size);
    }
    if (strcmp(key, "cflags") == 0) {
        return reject_duplicate(&seen->cflags, key, error, error_size) ||
               parse_string_list(value, &profile->cflags, error, error_size);
    }
    forge_util_set_error(error, error_size, "field '%s' is not allowed in this section", key);
    return -1;
}

static int parse_assignment(ForgeManifestSection section, char *line,
                            ForgeManifest *manifest, ForgeManifestSeen *seen,
                            char *error, size_t error_size)
{
    char *equals = strchr(line, '=');
    char *key;
    char *value;

    if (equals == NULL) {
        forge_util_set_error(error, error_size, "expected 'key = value'");
        return -1;
    }
    *equals = '\0';
    key = forge_util_trim(line);
    value = forge_util_trim(equals + 1);
    if (*key == '\0' || *value == '\0') {
        forge_util_set_error(error, error_size, "expected a non-empty key and value");
        return -1;
    }

    if (section == FORGE_SECTION_PROJECT && strcmp(key, "name") == 0) {
        return reject_duplicate(&seen->project_name, key, error, error_size) ||
               parse_scalar(value, manifest->project_name, sizeof(manifest->project_name),
                            error, error_size);
    }
    if (section == FORGE_SECTION_PROJECT && strcmp(key, "version") == 0) {
        char parsed[FORGE_MANIFEST_VALUE_MAX];

        if (reject_duplicate(&seen->project_version, key, error, error_size) != 0) {
            return -1;
        }
        if (parse_scalar(value, parsed, sizeof(parsed), error, error_size) != 0) {
            return -1;
        }
        if (!version_is_valid(parsed)) {
            forge_util_set_error(error, error_size,
                      "version '%s' must be MAJOR.MINOR.PATCH with an optional "
                      "-prerelease (e.g. \"1.2.3\" or \"1.2.3-rc.1\")", parsed);
            return -1;
        }
        (void)snprintf(manifest->project_version,
                       sizeof(manifest->project_version), "%s", parsed);
        return 0;
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "c") == 0) {
        return reject_duplicate(&seen->source_c, key, error, error_size) ||
               parse_string_list(value, &manifest->c_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "cpp") == 0) {
        return reject_duplicate(&seen->source_cpp, key, error, error_size) ||
               parse_string_list(value, &manifest->cpp_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "asm") == 0) {
        return reject_duplicate(&seen->source_asm, key, error, error_size) ||
               parse_string_list(value, &manifest->asm_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_TARGETS && strcmp(key, "os") == 0) {
        return reject_duplicate(&seen->target_os, key, error, error_size) ||
               parse_string_list(value, &manifest->target_os, error, error_size);
    }
    if (section == FORGE_SECTION_TARGETS && strcmp(key, "arch") == 0) {
        return reject_duplicate(&seen->target_arch, key, error, error_size) ||
               parse_string_list(value, &manifest->target_arch, error, error_size);
    }
    if (section == FORGE_SECTION_BUILD && strcmp(key, "compiler") == 0) {
        return reject_duplicate(&seen->compiler, key, error, error_size) ||
               parse_scalar(value, manifest->compiler_override,
                            sizeof(manifest->compiler_override), error, error_size);
    }
    if (section == FORGE_SECTION_DEPENDENCIES) {
        return parse_dependency_assignment(&manifest->dependencies, key, value,
                                           error, error_size);
    }
    if (section == FORGE_SECTION_OVERRIDES) {
        char parsed[FORGE_MANIFEST_VALUE_MAX];
        size_t index;

        if (!dependency_name_is_valid(key)) {
            forge_util_set_error(error, error_size,
                      "'%s' is not a valid override name; use letters, digits, "
                      "'-', '_', '.'", key);
            return -1;
        }
        if (parse_scalar(value, parsed, sizeof(parsed), error, error_size) != 0) {
            return -1;
        }
        if (!version_is_valid(parsed)) {
            forge_util_set_error(error, error_size,
                      "override '%s': version '%s' must be MAJOR.MINOR.PATCH "
                      "with an optional -prerelease", key, parsed);
            return -1;
        }
        for (index = 0U; index < manifest->override_count; ++index) {
            if (strcmp(manifest->overrides[index].name, key) == 0) {
                forge_util_set_error(error, error_size,
                          "duplicate override '%s'", key);
                return -1;
            }
        }
        if (manifest->override_count == FORGE_MANIFEST_MAX_OVERRIDES) {
            forge_util_set_error(error, error_size, "more than %u overrides",
                      (unsigned int)FORGE_MANIFEST_MAX_OVERRIDES);
            return -1;
        }
        (void)snprintf(manifest->overrides[manifest->override_count].name,
                       sizeof(manifest->overrides[0].name), "%s", key);
        (void)snprintf(manifest->overrides[manifest->override_count].version,
                       sizeof(manifest->overrides[0].version), "%s", parsed);
        ++manifest->override_count;
        return 0;
    }
    if (section == FORGE_SECTION_PROFILE_DEBUG) {
        return parse_profile_assignment(&seen->debug, &manifest->debug_profile,
                                        key, value, error, error_size);
    }
    if (section == FORGE_SECTION_PROFILE_RELEASE) {
        return parse_profile_assignment(&seen->release, &manifest->release_profile,
                                        key, value, error, error_size);
    }

    forge_util_set_error(error, error_size, "field '%s' is not allowed in this section", key);
    return -1;
}
int forge_manifest_load(const char *path, ForgeManifest *manifest,
                        char *error, size_t error_size)
{
    FILE *file;
    char line[FORGE_MANIFEST_LINE_MAX];
    unsigned long line_number = 0UL;
    unsigned int sections_seen = 0U;
    ForgeManifestSection section = FORGE_SECTION_NONE;
    ForgeManifestSeen seen = {0};

    if (path == NULL || manifest == NULL) {
        forge_util_set_error(error, error_size, "manifest path and output are required");
        return -1;
    }
    *manifest = (ForgeManifest){0};
    manifest->debug_profile.opt_level = -1;
    manifest->release_profile.opt_level = -1;
    /* Binary mode keeps ftell exact, which the embedded-NUL check below
     * relies on; \r\n endings are handled by the trim helpers instead. */
    file = fopen(path, "rb");
    if (file == NULL) {
        forge_util_set_error(error, error_size, "could not open manifest '%s'", path);
        return -1;
    }

    /*
     * A UTF-8 byte-order mark is accepted (editors on Windows add one
     * silently) but must be skipped before parsing; otherwise the first
     * section header would read as "\xEF\xBB\xBF[project]" and fail with a
     * confusing "unknown section".
     */
    {
        char prefix[3];
        size_t prefix_length = fread(prefix, 1U, sizeof(prefix), file);

        if (prefix_length == 3U && (unsigned char)prefix[0] == 0xEFU &&
            (unsigned char)prefix[1] == 0xBBU && (unsigned char)prefix[2] == 0xBFU) {
            /* BOM consumed; parsing continues after it. */
        } else if (fseek(file, 0L, SEEK_SET) != 0) {
            forge_util_set_error(error, error_size,
                      "could not rewind manifest '%s'", path);
            (void)fclose(file);
            return -1;
        }
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text;
        long position_before = ftell(file) - (long)strlen(line);
        long bytes_read = ftell(file) - position_before;

        ++line_number;
        /*
         * strlen must account for every byte fgets consumed; when it does
         * not, an embedded NUL truncated the C string silently — reject
         * binary contamination loudly instead of parsing half a line.
         */
        if ((long)strlen(line) != bytes_read || bytes_read <= 0L) {
            forge_util_set_error(error, error_size,
                      "%s:%lu: line contains an embedded NUL byte or invalid "
                      "binary content", path, line_number);
            (void)fclose(file);
            return -1;
        }
        if (strchr(line, '\n') == NULL && !feof(file)) {
            forge_util_set_error(error, error_size, "%s:%lu: line is too long", path, line_number);
            (void)fclose(file);
            return -1;
        }
        remove_comment(line);
        text = forge_util_trim(line);
        if (*text == '\0') {
            continue;
        }
        if (*text == '[') {
            unsigned int section_bit = 1U << (unsigned int)parse_section(text);

            section = parse_section(text);
            if (section == FORGE_SECTION_NONE) {
                forge_util_set_error(error, error_size, "%s:%lu: unknown section '%s'", path,
                          line_number, text);
                (void)fclose(file);
                return -1;
            }
            /*
             * TOML forbids repeating a table; silently merging two
             * [dependencies] blocks (say, one hand-written and one appended
             * by `forge add`) hides exactly the edits users need to see.
             */
            if ((sections_seen & section_bit) != 0U) {
                forge_util_set_error(error, error_size, "%s:%lu: duplicate section '%s'",
                          path, line_number, text);
                (void)fclose(file);
                return -1;
            }
            sections_seen |= section_bit;
            continue;
        }
        if (section == FORGE_SECTION_NONE) {
            forge_util_set_error(error, error_size, "%s:%lu: field without a section header; "
                                  "expected a [section] line first", path, line_number);
            (void)fclose(file);
            return -1;
        }
        if (parse_assignment(section, text, manifest, &seen, error, error_size) != 0) {
            char detail[FORGE_MANIFEST_LINE_MAX];
            if (error == NULL || error[0] == '\0') {
                (void)snprintf(detail, sizeof(detail), "invalid field");
            } else {
                (void)snprintf(detail, sizeof(detail), "%s", error);
            }
            forge_util_set_error(error, error_size, "%s:%lu: %s", path, line_number, detail);
            (void)fclose(file);
            return -1;
        }
    }
    (void)fclose(file);

    if (!seen.project_name || !seen.target_os || !seen.target_arch) {
        forge_util_set_error(error, error_size,
                  "%s: manifest requires project.name and targets.os/arch", path);
        return -1;
    }
    if (manifest->c_source_dirs.count == 0U && manifest->cpp_source_dirs.count == 0U &&
        manifest->asm_source_dirs.count == 0U) {
        forge_util_set_error(error, error_size, "%s: at least one source directory is required", path);
        return -1;
    }
    if (manifest->target_os.count == 0U || manifest->target_arch.count == 0U) {
        forge_util_set_error(error, error_size, "%s: targets.os and targets.arch cannot be empty", path);
        return -1;
    }
    return 0;
}
