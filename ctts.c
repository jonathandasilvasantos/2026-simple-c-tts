/*
 * CTTS - Concatenative Text-to-Speech Engine
 * Main implementation file
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <regex.h>

#include "ctts.h"

/* ============================================================================
 * Normalization Rules (loaded from CSV)
 * ============================================================================ */

#define MAX_NORM_RULES 256
#define MAX_REPLACE_LEN 256

typedef struct {
    regex_t regex;
    char replace[MAX_REPLACE_LEN];
    int compiled;
} NormRule;

static NormRule norm_rules[MAX_NORM_RULES];
static size_t norm_rule_count = 0;
static int norm_rules_loaded = 0;

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

#define FNV_OFFSET_BASIS    2166136261U
#define FNV_PRIME           16777619U
#define HASH_TABLE_LOAD     0.7
#define PI                  3.14159265358979323846
#define OVERLAP_SAMPLES(ms) ((int)((ms) * CTTS_SAMPLE_RATE / 1000.0f))

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

/* WAV file header */
typedef struct {
    char riff[4];           /* "RIFF" */
    uint32_t file_size;     /* File size - 8 */
    char wave[4];           /* "WAVE" */
} WAVRiff;

typedef struct {
    char id[4];             /* Chunk ID */
    uint32_t size;          /* Chunk size */
} WAVChunk;

typedef struct {
    uint16_t audio_format;  /* 1 = PCM */
    uint16_t num_channels;  /* 1 = mono */
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} WAVFmt;

/* Unit for building database */
typedef struct {
    char* text;
    size_t text_len;
    size_t char_count;
    int16_t* samples;
    size_t sample_count;
    uint32_t hash;
} BuildUnit;

/* ============================================================================
 * Error Messages
 * ============================================================================ */

static const char* error_messages[] = {
    "Success",
    "Invalid argument",
    "File not found",
    "File read error",
    "File write error",
    "Invalid format",
    "Out of memory",
    "Invalid WAV file",
    "Version mismatch"
};

const char* ctts_strerror(int error_code) {
    if (error_code >= 0) return error_messages[0];
    int idx = -error_code;
    if (idx >= (int)(sizeof(error_messages) / sizeof(error_messages[0]))) {
        return "Unknown error";
    }
    return error_messages[idx];
}

/* ============================================================================
 * UTF-8 Utilities
 * ============================================================================ */

size_t ctts_utf8_strlen(const char* str) {
    size_t count = 0;
    while (*str) {
        if ((*str & 0xC0) != 0x80) count++;
        str++;
    }
    return count;
}

uint32_t ctts_utf8_next(const char** str) {
    const unsigned char* s = (const unsigned char*)*str;
    uint32_t cp;

    if (*s < 0x80) {
        cp = *s++;
    } else if ((*s & 0xE0) == 0xC0) {
        cp = (*s++ & 0x1F) << 6;
        if ((*s & 0xC0) == 0x80) cp |= *s++ & 0x3F;
    } else if ((*s & 0xF0) == 0xE0) {
        cp = (*s++ & 0x0F) << 12;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F) << 6;
        if ((*s & 0xC0) == 0x80) cp |= *s++ & 0x3F;
    } else if ((*s & 0xF8) == 0xF0) {
        cp = (*s++ & 0x07) << 18;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F) << 12;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F) << 6;
        if ((*s & 0xC0) == 0x80) cp |= *s++ & 0x3F;
    } else {
        cp = '?';
        s++;
    }

    *str = (const char*)s;
    return cp;
}

/* Get UTF-8 byte length of next character */
static int utf8_char_len(const char* str) {
    unsigned char c = (unsigned char)*str;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* ============================================================================
 * Hash Functions
 * ============================================================================ */

uint32_t ctts_hash(const char* str, size_t len) {
    uint32_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)str[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

/* ============================================================================
 * Text Normalization
 * ============================================================================ */

/* Simple lowercase for ASCII and common accented chars */
static uint32_t unicode_tolower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    /* Common accented uppercase to lowercase */
    if (cp == 0xC9) return 0xE9;  /* É -> é */
    if (cp == 0xD3) return 0xF3;  /* Ó -> ó */
    if (cp == 0xD4) return 0xF4;  /* Ô -> ô */
    if (cp == 0xC7) return 0xE7;  /* Ç -> ç */
    return cp;
}

/* Encode codepoint to UTF-8 */
static int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

char* ctts_normalize(const char* text) {
    size_t len = strlen(text);
    char* result = malloc(len * 4 + 1);  /* Worst case expansion */
    if (!result) return NULL;

    const char* src = text;
    char* dst = result;

    while (*src) {
        uint32_t cp = ctts_utf8_next(&src);
        cp = unicode_tolower(cp);
        dst += utf8_encode(cp, dst);
    }
    *dst = '\0';

    return result;
}

/* ============================================================================
 * Normalization Rules from CSV
 * ============================================================================ */

/* Convert portable \b word boundary to platform-specific syntax */
static char* convert_word_boundaries(const char* pattern) {
    /* Count how many \b we need to replace */
    size_t count = 0;
    const char* p = pattern;
    while ((p = strstr(p, "\\b")) != NULL) {
        count++;
        p += 2;
    }

    if (count == 0) {
        return strdup(pattern);
    }

    /* [[:<:]] and [[:>:]] are 7 chars, \b is 2 chars, so we need 5 extra per replacement */
    size_t new_len = strlen(pattern) + count * 5 + 1;
    char* result = malloc(new_len);
    if (!result) return strdup(pattern);

    const char* src = pattern;
    char* dst = result;

    while (*src) {
        if (src[0] == '\\' && src[1] == 'b') {
            /* Check context: if followed by alphanumeric, it's word start
               otherwise it's word end */
            char next_char = src[2];
            if ((next_char >= 'a' && next_char <= 'z') ||
                (next_char >= 'A' && next_char <= 'Z') ||
                (next_char >= '0' && next_char <= '9') ||
                next_char == '[' || next_char == '(') {
                /* Word start boundary */
                memcpy(dst, "[[:<:]]", 7);
                dst += 7;
            } else {
                /* Word end boundary */
                memcpy(dst, "[[:>:]]", 7);
                dst += 7;
            }
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    return result;
}

/* Load normalization rules from CSV file */
int ctts_load_normalization(const char* csv_file) {
    if (norm_rules_loaded) {
        /* Already loaded, skip */
        return CTTS_OK;
    }

    FILE* f = fopen(csv_file, "r");
    if (!f) {
        /* File not found is OK - just no rules */
        norm_rules_loaded = 1;
        return CTTS_OK;
    }

    char line[512];
    norm_rule_count = 0;

    while (fgets(line, sizeof(line), f) && norm_rule_count < MAX_NORM_RULES) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        /* Skip empty lines and comments */
        if (len == 0) continue;
        if (line[0] == '#') continue;

        /* Find the comma separator */
        char* comma = strchr(line, ',');
        if (!comma) continue;

        /* Split into pattern and replacement */
        *comma = '\0';
        const char* pattern = line;
        const char* replace = comma + 1;

        /* Convert portable \b to platform-specific word boundaries */
        char* converted_pattern = convert_word_boundaries(pattern);
        if (!converted_pattern) continue;

        /* Compile the regex */
        NormRule* rule = &norm_rules[norm_rule_count];
        int err = regcomp(&rule->regex, converted_pattern, REG_EXTENDED);
        if (err != 0) {
            fprintf(stderr, "Warning: Invalid regex pattern '%s' (converted from '%s')\n",
                    converted_pattern, pattern);
            free(converted_pattern);
            continue;
        }
        free(converted_pattern);

        strncpy(rule->replace, replace, MAX_REPLACE_LEN - 1);
        rule->replace[MAX_REPLACE_LEN - 1] = '\0';
        rule->compiled = 1;
        norm_rule_count++;
    }

    fclose(f);
    norm_rules_loaded = 1;

    if (norm_rule_count > 0) {
        fprintf(stderr, "Loaded %zu normalization rules\n", norm_rule_count);
    }

    return CTTS_OK;
}

/* Apply replacement with backreference support (\1, \2, etc.) */
static size_t apply_replacement(char* dst, size_t dst_remaining,
                                const char* replace, const char* src,
                                regmatch_t* matches, size_t nmatch) {
    size_t written = 0;
    const char* r = replace;

    while (*r && written < dst_remaining) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '9') {
            /* Backreference */
            size_t group = r[1] - '0';
            if (group < nmatch && matches[group].rm_so >= 0) {
                size_t group_len = matches[group].rm_eo - matches[group].rm_so;
                if (group_len > dst_remaining - written) {
                    group_len = dst_remaining - written;
                }
                memcpy(dst + written, src + matches[group].rm_so, group_len);
                written += group_len;
            }
            r += 2;
        } else {
            dst[written++] = *r++;
        }
    }

    return written;
}

/* Apply normalization rules to text */
char* ctts_apply_normalization(const char* text) {
    if (norm_rule_count == 0) {
        return strdup(text);
    }

    /* Work buffer - start with copy of input */
    size_t buf_size = strlen(text) * 4 + 1024;  /* Extra space for expansions */
    char* current = malloc(buf_size);
    char* next = malloc(buf_size);
    if (!current || !next) {
        free(current);
        free(next);
        return strdup(text);
    }

    strcpy(current, text);

    /* Apply each rule */
    for (size_t i = 0; i < norm_rule_count; i++) {
        NormRule* rule = &norm_rules[i];
        if (!rule->compiled) continue;

        #define MAX_GROUPS 10
        regmatch_t matches[MAX_GROUPS];
        char* src = current;
        char* dst = next;
        size_t dst_remaining = buf_size - 1;

        while (*src && dst_remaining > 0) {
            if (regexec(&rule->regex, src, MAX_GROUPS, matches, 0) == 0 && matches[0].rm_so >= 0) {
                /* Copy text before match */
                size_t before_len = (size_t)matches[0].rm_so;
                if (before_len > dst_remaining) before_len = dst_remaining;
                memcpy(dst, src, before_len);
                dst += before_len;
                dst_remaining -= before_len;

                /* Apply replacement with backreference support */
                size_t replace_len = apply_replacement(dst, dst_remaining,
                                                       rule->replace, src,
                                                       matches, MAX_GROUPS);
                dst += replace_len;
                dst_remaining -= replace_len;

                /* Advance past matched portion */
                src += matches[0].rm_eo;
                if (matches[0].rm_eo == 0) src++;  /* Avoid infinite loop on zero-length match */
            } else {
                /* No more matches, copy rest */
                size_t rest_len = strlen(src);
                if (rest_len > dst_remaining) rest_len = dst_remaining;
                memcpy(dst, src, rest_len);
                dst += rest_len;
                break;
            }
        }
        *dst = '\0';

        /* Swap buffers for next iteration */
        char* tmp = current;
        current = next;
        next = tmp;
    }

    free(next);
    return current;
}

/* Free normalization rules */
void ctts_free_normalization(void) {
    for (size_t i = 0; i < norm_rule_count; i++) {
        if (norm_rules[i].compiled) {
            regfree(&norm_rules[i].regex);
            norm_rules[i].compiled = 0;
        }
    }
    norm_rule_count = 0;
    norm_rules_loaded = 0;
}

/* ============================================================================
 * WAV File I/O
 * ============================================================================ */

/* Read WAV file and extract PCM samples */
static int read_wav(const char* path, int16_t** samples, size_t* sample_count) {
    FILE* f = fopen(path, "rb");
    if (!f) return CTTS_ERR_FILE_NOT_FOUND;

    WAVRiff riff;
    if (fread(&riff, sizeof(riff), 1, f) != 1) {
        fclose(f);
        return CTTS_ERR_FILE_READ;
    }

    if (memcmp(riff.riff, "RIFF", 4) != 0 || memcmp(riff.wave, "WAVE", 4) != 0) {
        fclose(f);
        return CTTS_ERR_INVALID_WAV;
    }

    WAVFmt fmt = {0};
    int found_fmt = 0, found_data = 0;
    uint32_t data_size = 0;

    while (!found_data) {
        WAVChunk chunk;
        if (fread(&chunk, sizeof(chunk), 1, f) != 1) break;

        if (memcmp(chunk.id, "fmt ", 4) == 0) {
            if (chunk.size < sizeof(fmt)) {
                fclose(f);
                return CTTS_ERR_INVALID_WAV;
            }
            if (fread(&fmt, sizeof(fmt), 1, f) != 1) {
                fclose(f);
                return CTTS_ERR_FILE_READ;
            }
            /* Skip extra bytes in fmt chunk */
            if (chunk.size > sizeof(fmt)) {
                fseek(f, chunk.size - sizeof(fmt), SEEK_CUR);
            }
            found_fmt = 1;
        } else if (memcmp(chunk.id, "data", 4) == 0) {
            data_size = chunk.size;
            found_data = 1;
        } else {
            /* Skip unknown chunk */
            fseek(f, chunk.size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        fclose(f);
        return CTTS_ERR_INVALID_WAV;
    }

    if (fmt.audio_format != 1 || fmt.bits_per_sample != 16) {
        fclose(f);
        return CTTS_ERR_INVALID_WAV;
    }

    *sample_count = data_size / (fmt.bits_per_sample / 8) / fmt.num_channels;
    *samples = malloc(*sample_count * sizeof(int16_t));
    if (!*samples) {
        fclose(f);
        return CTTS_ERR_OUT_OF_MEMORY;
    }

    /* Read samples (handle mono/stereo) */
    if (fmt.num_channels == 1) {
        if (fread(*samples, sizeof(int16_t), *sample_count, f) != *sample_count) {
            free(*samples);
            fclose(f);
            return CTTS_ERR_FILE_READ;
        }
    } else {
        /* Stereo: average channels */
        for (size_t i = 0; i < *sample_count; i++) {
            int16_t left, right;
            if (fread(&left, sizeof(int16_t), 1, f) != 1 ||
                fread(&right, sizeof(int16_t), 1, f) != 1) {
                free(*samples);
                fclose(f);
                return CTTS_ERR_FILE_READ;
            }
            (*samples)[i] = (left + right) / 2;
        }
    }

    fclose(f);
    return CTTS_OK;
}

int ctts_write_wav(const char* filename, const int16_t* samples,
                   size_t sample_count, int sample_rate) {
    FILE* f = fopen(filename, "wb");
    if (!f) return CTTS_ERR_FILE_WRITE;

    uint32_t data_size = sample_count * sizeof(int16_t);
    uint32_t file_size = 36 + data_size;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);

    uint16_t audio_format = 1;  /* PCM */
    uint16_t num_channels = 1;  /* Mono */
    uint32_t sr = sample_rate;
    uint32_t byte_rate = sample_rate * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;

    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sr, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    fwrite(samples, sizeof(int16_t), sample_count, f);

    fclose(f);
    return CTTS_OK;
}

/* ============================================================================
 * Database Building
 * ============================================================================ */

/* Parse index file and load units */
static int load_units_from_index(const char* wav_dir, const char* index_file,
                                 BuildUnit** units, size_t* count) {
    FILE* f = fopen(index_file, "r");
    if (!f) return CTTS_ERR_FILE_NOT_FOUND;

    /* Count lines first */
    size_t capacity = 1024;
    *units = malloc(capacity * sizeof(BuildUnit));
    if (!*units) {
        fclose(f);
        return CTTS_ERR_OUT_OF_MEMORY;
    }
    *count = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Skip empty lines */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        /* Parse: filename|text|display */
        char* filename = strtok(line, "|");
        char* text = strtok(NULL, "|");
        if (!filename || !text) continue;

        /* Build full path */
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s.wav", wav_dir, filename);

        /* Load WAV */
        int16_t* samples;
        size_t sample_count;
        int err = read_wav(path, &samples, &sample_count);
        if (err != CTTS_OK) {
            fprintf(stderr, "Warning: Could not load %s: %s\n",
                    path, ctts_strerror(err));
            continue;
        }

        /* Normalize text */
        char* normalized = ctts_normalize(text);
        if (!normalized) {
            free(samples);
            continue;
        }

        /* Grow array if needed */
        if (*count >= capacity) {
            capacity *= 2;
            BuildUnit* new_units = realloc(*units, capacity * sizeof(BuildUnit));
            if (!new_units) {
                free(samples);
                free(normalized);
                continue;
            }
            *units = new_units;
        }

        BuildUnit* unit = &(*units)[*count];
        unit->text = normalized;
        unit->text_len = strlen(normalized);
        unit->char_count = ctts_utf8_strlen(normalized);
        unit->samples = samples;
        unit->sample_count = sample_count;
        unit->hash = ctts_hash(normalized, unit->text_len);

        (*count)++;
    }

    fclose(f);
    return CTTS_OK;
}

/* Compare units by character count (descending) for longest-match */
static int compare_units(const void* a, const void* b) {
    const BuildUnit* ua = (const BuildUnit*)a;
    const BuildUnit* ub = (const BuildUnit*)b;
    if (ub->char_count != ua->char_count)
        return (int)ub->char_count - (int)ua->char_count;
    return strcmp(ua->text, ub->text);
}

int ctts_build_database(const char* letters_dir, const char* letters_index,
                        const char* syllables_dir, const char* syllables_index,
                        const char* output_file) {
    BuildUnit* letters = NULL;
    BuildUnit* syllables = NULL;
    size_t letter_count = 0, syllable_count = 0;
    int err;

    /* Load letters */
    err = load_units_from_index(letters_dir, letters_index, &letters, &letter_count);
    if (err != CTTS_OK) {
        fprintf(stderr, "Failed to load letters: %s\n", ctts_strerror(err));
        return err;
    }
    printf("Loaded %zu letters\n", letter_count);

    /* Load syllables */
    err = load_units_from_index(syllables_dir, syllables_index, &syllables, &syllable_count);
    if (err != CTTS_OK) {
        fprintf(stderr, "Failed to load syllables: %s\n", ctts_strerror(err));
        /* Continue with just letters */
    } else {
        printf("Loaded %zu syllables\n", syllable_count);
    }

    /* Merge and sort */
    size_t total_count = letter_count + syllable_count;
    BuildUnit* all_units = malloc(total_count * sizeof(BuildUnit));
    if (!all_units) {
        err = CTTS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    memcpy(all_units, letters, letter_count * sizeof(BuildUnit));
    memcpy(all_units + letter_count, syllables, syllable_count * sizeof(BuildUnit));
    qsort(all_units, total_count, sizeof(BuildUnit), compare_units);

    /* Calculate sizes */
    size_t strings_size = 0;
    size_t audio_samples = 0;
    size_t max_chars = 0;

    for (size_t i = 0; i < total_count; i++) {
        strings_size += all_units[i].text_len + 1;
        audio_samples += all_units[i].sample_count;
        if (all_units[i].char_count > max_chars)
            max_chars = all_units[i].char_count;
    }

    /* Calculate hash table size (next power of 2, with load factor) */
    size_t hash_table_size = 1;
    while (hash_table_size < total_count / HASH_TABLE_LOAD)
        hash_table_size *= 2;

    /* Create file */
    FILE* out = fopen(output_file, "wb");
    if (!out) {
        err = CTTS_ERR_FILE_WRITE;
        goto cleanup;
    }

    /* Calculate offsets */
    size_t index_offset = sizeof(CTTSHeader);
    size_t hash_table_offset = index_offset + total_count * sizeof(CTTSIndexEntry);
    size_t strings_offset = hash_table_offset + hash_table_size * sizeof(uint32_t);
    size_t audio_offset = strings_offset + strings_size;

    /* Write header */
    CTTSHeader header = {
        .magic = CTTS_MAGIC,
        .version = CTTS_VERSION,
        .unit_count = (uint32_t)total_count,
        .sample_rate = CTTS_SAMPLE_RATE,
        .bits_per_sample = CTTS_BITS_PER_SAMPLE,
        .index_offset = (uint32_t)index_offset,
        .strings_offset = (uint32_t)strings_offset,
        .audio_offset = (uint32_t)audio_offset,
        .total_samples = (uint32_t)audio_samples,
        .max_unit_chars = (uint32_t)max_chars,
        .hash_table_size = (uint32_t)hash_table_size,
        .hash_table_offset = (uint32_t)hash_table_offset
    };
    fwrite(&header, sizeof(header), 1, out);

    /* Build index and write */
    CTTSIndexEntry* index = calloc(total_count, sizeof(CTTSIndexEntry));
    uint32_t* hash_table = calloc(hash_table_size, sizeof(uint32_t));
    if (!index || !hash_table) {
        free(index);
        free(hash_table);
        fclose(out);
        err = CTTS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    /* Initialize hash table to "empty" (-1) */
    memset(hash_table, 0xFF, hash_table_size * sizeof(uint32_t));

    size_t string_pos = 0;
    size_t audio_pos = 0;

    for (size_t i = 0; i < total_count; i++) {
        BuildUnit* unit = &all_units[i];
        CTTSIndexEntry* entry = &index[i];

        entry->hash = unit->hash;
        entry->string_offset = (uint32_t)string_pos;
        entry->string_len = (uint16_t)unit->text_len;
        entry->char_count = (uint16_t)unit->char_count;
        entry->audio_offset = (uint32_t)audio_pos;
        entry->sample_count = (uint32_t)unit->sample_count;
        entry->next_hash = 0xFFFFFFFF;

        /* Insert into hash table with chaining */
        uint32_t slot = unit->hash % hash_table_size;
        if (hash_table[slot] == 0xFFFFFFFF) {
            hash_table[slot] = (uint32_t)i;
        } else {
            /* Chain */
            uint32_t prev = hash_table[slot];
            while (index[prev].next_hash != 0xFFFFFFFF)
                prev = index[prev].next_hash;
            index[prev].next_hash = (uint32_t)i;
        }

        string_pos += unit->text_len + 1;
        audio_pos += unit->sample_count;
    }

    fwrite(index, sizeof(CTTSIndexEntry), total_count, out);
    fwrite(hash_table, sizeof(uint32_t), hash_table_size, out);

    /* Write string pool */
    for (size_t i = 0; i < total_count; i++) {
        fwrite(all_units[i].text, 1, all_units[i].text_len + 1, out);
    }

    /* Write audio data */
    for (size_t i = 0; i < total_count; i++) {
        fwrite(all_units[i].samples, sizeof(int16_t),
               all_units[i].sample_count, out);
    }

    fclose(out);
    free(index);
    free(hash_table);

    printf("Database written to %s\n", output_file);
    printf("  Units: %zu\n", total_count);
    printf("  Max unit length: %zu characters\n", max_chars);
    printf("  Total audio samples: %zu\n", audio_samples);

    err = CTTS_OK;

cleanup:
    /* Free units */
    for (size_t i = 0; i < letter_count; i++) {
        free(letters[i].text);
        free(letters[i].samples);
    }
    free(letters);

    for (size_t i = 0; i < syllable_count; i++) {
        free(syllables[i].text);
        free(syllables[i].samples);
    }
    free(syllables);

    if (all_units != letters && all_units != syllables)
        free(all_units);

    return err;
}

/* ============================================================================
 * Engine Initialization
 * ============================================================================ */

CTTS* ctts_init(const char* database_file) {
    CTTS* engine = calloc(1, sizeof(CTTS));
    if (!engine) return NULL;

    /* Open and map file */
    engine->db_fd = open(database_file, O_RDONLY);
    if (engine->db_fd < 0) {
        free(engine);
        return NULL;
    }

    struct stat st;
    if (fstat(engine->db_fd, &st) < 0) {
        close(engine->db_fd);
        free(engine);
        return NULL;
    }
    engine->db_size = st.st_size;

    engine->db_data = mmap(NULL, engine->db_size, PROT_READ, MAP_PRIVATE,
                           engine->db_fd, 0);
    if (engine->db_data == MAP_FAILED) {
        close(engine->db_fd);
        free(engine);
        return NULL;
    }

    /* Parse header */
    memcpy(&engine->header, engine->db_data, sizeof(CTTSHeader));

    if (engine->header.magic != CTTS_MAGIC ||
        engine->header.version != CTTS_VERSION) {
        munmap(engine->db_data, engine->db_size);
        close(engine->db_fd);
        free(engine);
        return NULL;
    }

    /* Set up pointers */
    engine->index = (CTTSIndexEntry*)(engine->db_data + engine->header.index_offset);
    engine->hash_table = (uint32_t*)(engine->db_data + engine->header.hash_table_offset);
    engine->strings = (char*)(engine->db_data + engine->header.strings_offset);
    engine->audio = (int16_t*)(engine->db_data + engine->header.audio_offset);

    /* Load config with defaults */
    ctts_config_defaults(&engine->config);

    return engine;
}

void ctts_free(CTTS* engine) {
    if (!engine) return;

    if (engine->db_data && engine->db_data != MAP_FAILED) {
        munmap(engine->db_data, engine->db_size);
    }
    if (engine->db_fd >= 0) {
        close(engine->db_fd);
    }
    free(engine);

    /* Clean up normalization rules */
    ctts_free_normalization();
}

void ctts_free_samples(int16_t* samples) {
    free(samples);
}

/* ============================================================================
 * Configuration
 * ============================================================================ */

void ctts_config_defaults(CTTSConfig* config) {
    config->crossfade_ms = CTTS_DEFAULT_CROSSFADE_MS;
    config->crossfade_vowel_ms = 45.0f;  /* Longer crossfade for vowel endings */
    config->crossfade_s_ending_ms = 30.0f;  /* Shorter crossfade for S endings */
    config->crossfade_r_ending_ms = 30.0f;  /* Shorter crossfade for R endings */
    config->vowel_to_consonant_factor = 0.5f;  /* 50% shorter for vowel-to-consonant */
    config->word_pause_ms = CTTS_DEFAULT_WORD_PAUSE_MS;
    config->remove_word_silence = 1;     /* Remove silence within words */
    config->silence_threshold = 0.02f;   /* 2% of max amplitude */
    config->min_silence_ms = 15.0f;      /* 15ms minimum silence to detect */
    config->unknown_silence_ms = CTTS_DEFAULT_UNKNOWN_SILENCE_MS;
    config->fade_in_ms = CTTS_DEFAULT_FADE_IN_MS;
    config->fade_out_ms = CTTS_DEFAULT_FADE_OUT_MS;
    config->remove_dc_offset = 1;
    config->normalize_level = 0.0f;
    config->compression = 0.0f;
    config->default_speed = CTTS_DEFAULT_SPEED;
    config->min_speed = CTTS_MIN_SPEED;
    config->max_speed = CTTS_MAX_SPEED;
    config->print_units = 0;
    config->print_timing = 0;
}

/* Simple YAML-like config parser */
static void parse_config_line(CTTSConfig* config, const char* line) {
    /* Skip comments and empty lines */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == '\0' || *line == '\n') return;

    /* Parse key: value */
    char key[64] = {0};
    char value[64] = {0};

    /* Find colon */
    const char* colon = strchr(line, ':');
    if (!colon) return;

    /* Extract key */
    size_t key_len = colon - line;
    if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
    strncpy(key, line, key_len);
    key[key_len] = '\0';

    /* Trim key */
    char* k = key;
    while (*k == ' ' || *k == '\t') k++;
    char* end = k + strlen(k) - 1;
    while (end > k && (*end == ' ' || *end == '\t')) *end-- = '\0';

    /* Extract value */
    const char* v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    strncpy(value, v, sizeof(value) - 1);
    end = value + strlen(value) - 1;
    while (end > value && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';

    /* Match key and set value */
    if (strcmp(k, "crossfade_ms") == 0) {
        config->crossfade_ms = strtof(value, NULL);
    } else if (strcmp(k, "crossfade_vowel_ms") == 0) {
        config->crossfade_vowel_ms = strtof(value, NULL);
    } else if (strcmp(k, "crossfade_s_ending_ms") == 0) {
        config->crossfade_s_ending_ms = strtof(value, NULL);
    } else if (strcmp(k, "crossfade_r_ending_ms") == 0) {
        config->crossfade_r_ending_ms = strtof(value, NULL);
    } else if (strcmp(k, "vowel_to_consonant_factor") == 0) {
        config->vowel_to_consonant_factor = strtof(value, NULL);
    } else if (strcmp(k, "word_pause_ms") == 0) {
        config->word_pause_ms = strtof(value, NULL);
    } else if (strcmp(k, "unknown_silence_ms") == 0) {
        config->unknown_silence_ms = strtof(value, NULL);
    } else if (strcmp(k, "fade_in_ms") == 0) {
        config->fade_in_ms = strtof(value, NULL);
    } else if (strcmp(k, "fade_out_ms") == 0) {
        config->fade_out_ms = strtof(value, NULL);
    } else if (strcmp(k, "remove_word_silence") == 0) {
        config->remove_word_silence = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(k, "silence_threshold") == 0) {
        config->silence_threshold = strtof(value, NULL);
    } else if (strcmp(k, "min_silence_ms") == 0) {
        config->min_silence_ms = strtof(value, NULL);
    } else if (strcmp(k, "remove_dc_offset") == 0) {
        config->remove_dc_offset = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(k, "normalize_level") == 0) {
        config->normalize_level = strtof(value, NULL);
    } else if (strcmp(k, "compression") == 0) {
        config->compression = strtof(value, NULL);
    } else if (strcmp(k, "default_speed") == 0) {
        config->default_speed = strtof(value, NULL);
    } else if (strcmp(k, "min_speed") == 0) {
        config->min_speed = strtof(value, NULL);
    } else if (strcmp(k, "max_speed") == 0) {
        config->max_speed = strtof(value, NULL);
    } else if (strcmp(k, "print_units") == 0) {
        config->print_units = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(k, "print_timing") == 0) {
        config->print_timing = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    }
}

int ctts_load_config(CTTSConfig* config, const char* config_file) {
    /* Start with defaults */
    ctts_config_defaults(config);

    FILE* f = fopen(config_file, "r");
    if (!f) {
        /* Config file not found, use defaults */
        return CTTS_OK;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        parse_config_line(config, line);
    }

    fclose(f);
    return CTTS_OK;
}

void ctts_set_crossfade(CTTS* engine, float crossfade_ms) {
    if (engine) engine->config.crossfade_ms = crossfade_ms;
}

void ctts_set_word_pause(CTTS* engine, float pause_ms) {
    if (engine) engine->config.word_pause_ms = pause_ms;
}

void ctts_set_unknown_silence(CTTS* engine, float silence_ms) {
    if (engine) engine->config.unknown_silence_ms = silence_ms;
}

void ctts_set_fades(CTTS* engine, float fade_in_ms, float fade_out_ms) {
    if (engine) {
        engine->config.fade_in_ms = fade_in_ms;
        engine->config.fade_out_ms = fade_out_ms;
    }
}

/* ============================================================================
 * Unit Lookup
 * ============================================================================ */

/* Find unit by text, returns index or -1 if not found */
static int find_unit(CTTS* engine, const char* text, size_t len) {
    uint32_t hash = ctts_hash(text, len);
    uint32_t slot = hash % engine->header.hash_table_size;
    uint32_t idx = engine->hash_table[slot];

    while (idx != 0xFFFFFFFF) {
        CTTSIndexEntry* entry = &engine->index[idx];
        if (entry->hash == hash && entry->string_len == len) {
            const char* unit_text = engine->strings + entry->string_offset;
            if (memcmp(unit_text, text, len) == 0) {
                return (int)idx;
            }
        }
        idx = entry->next_hash;
    }

    return -1;
}

/* Find the longest matching unit starting at pos, returns byte length or 0 */
static size_t find_longest_match(CTTS* engine, const char* pos, size_t max_chars) {
    size_t remaining = strlen(pos);
    size_t try_chars = max_chars;
    if (try_chars > remaining) try_chars = remaining;

    /* Calculate byte position for try_chars characters */
    const char* end = pos;
    for (size_t c = 0; c < try_chars && *end; c++) {
        end += utf8_char_len(end);
    }

    /* Try decreasing lengths */
    while (end > pos) {
        size_t try_len = end - pos;
        if (find_unit(engine, pos, try_len) >= 0) {
            return try_len;
        }

        /* Move back one character */
        const char* prev_end = pos;
        const char* scan = pos;
        while (scan < end) {
            prev_end = scan;
            scan += utf8_char_len(scan);
            if (scan >= end) break;
        }
        end = prev_end;
    }

    return 0;
}

/* Forward declarations for Portuguese rules */
static int pt_reject_single_consonant(const char* pos, size_t match_char_count,
                                       int at_word_start);
static int pt_syllable_score(const char* text, size_t byte_len, size_t char_count,
                              int at_word_start);

/*
 * Greedy syllable matching with look-ahead and Portuguese rules.
 * For each position, considers all possible matches and picks the one
 * that follows Portuguese syllable rules and leads to the best next match.
 *
 * Portuguese rules applied:
 * - Single consonants at word start are rejected (need CV minimum)
 * - Digraphs (ch, lh, nh, qu, gu) are kept together
 * - Valid consonant clusters (pr, br, tr, etc.) are preferred
 * - Open syllables (ending in vowel) are preferred
 */
static size_t find_best_match_with_lookahead(CTTS* engine, const char* pos,
                                              size_t max_chars, int* out_unit_idx,
                                              int at_word_start) {
    size_t remaining_bytes = strlen(pos);
    if (remaining_bytes == 0) {
        *out_unit_idx = -1;
        return 0;
    }

    /* Count remaining characters */
    size_t remaining_chars = 0;
    const char* tmp = pos;
    while (*tmp) {
        remaining_chars++;
        tmp += utf8_char_len(tmp);
    }

    size_t try_chars = max_chars;
    if (try_chars > remaining_chars) try_chars = remaining_chars;

    /* Collect all possible matches at current position */
    typedef struct {
        size_t byte_len;
        size_t char_count;
        int unit_idx;
        size_t next_match_len;  /* Length of best match at next position */
        int pt_score;           /* Portuguese syllable quality score */
    } MatchCandidate;

    MatchCandidate candidates[64];  /* Max candidates to consider */
    size_t num_candidates = 0;

    /* Build list of all matches from longest to shortest */
    const char* end = pos;
    for (size_t c = 0; c < try_chars && *end; c++) {
        end += utf8_char_len(end);
    }

    size_t char_count = try_chars;
    while (end > pos && num_candidates < 64) {
        size_t try_len = end - pos;
        int unit_idx = find_unit(engine, pos, try_len);

        if (unit_idx >= 0) {
            /* Apply Portuguese rules: reject invalid single consonants */
            if (!pt_reject_single_consonant(pos, char_count, at_word_start)) {
                candidates[num_candidates].byte_len = try_len;
                candidates[num_candidates].char_count = char_count;
                candidates[num_candidates].unit_idx = unit_idx;
                candidates[num_candidates].next_match_len = 0;
                candidates[num_candidates].pt_score = pt_syllable_score(
                    pos, try_len, char_count, at_word_start);
                num_candidates++;
            }
        }

        /* Move back one character */
        const char* prev_end = pos;
        const char* scan = pos;
        while (scan < end) {
            prev_end = scan;
            scan += utf8_char_len(scan);
            if (scan >= end) break;
        }
        end = prev_end;
        char_count--;
    }

    if (num_candidates == 0) {
        *out_unit_idx = -1;
        return 0;
    }

    /* If only one candidate, return it */
    if (num_candidates == 1) {
        *out_unit_idx = candidates[0].unit_idx;
        return candidates[0].byte_len;
    }

    /* For each candidate, find the longest match at the next position */
    for (size_t i = 0; i < num_candidates; i++) {
        const char* next_pos = pos + candidates[i].byte_len;
        /* Skip whitespace for lookahead */
        while (*next_pos == ' ' || *next_pos == '\t' || *next_pos == '\n') {
            next_pos++;
        }
        if (*next_pos) {
            candidates[i].next_match_len = find_longest_match(engine, next_pos, max_chars);
        }
    }

    /* Select best candidate using Portuguese syllable rules:
     * 1. Primary: Portuguese syllable quality score (pt_score)
     * 2. Secondary: maximize (current_chars + next_match_chars)
     * 3. Tie-breaking:
     *    - If at end of word, prefer longer current match
     *    - Otherwise, prefer longer next match (better syllable boundaries)
     *
     * Portuguese rules ensure:
     * - No single consonant at word start (CV minimum)
     * - Digraphs (ch, lh, nh, qu, gu) stay together
     * - Valid clusters (pr, br, etc.) are preferred
     */
    size_t best_idx = 0;
    int best_pt_score = candidates[0].pt_score;
    size_t best_total = candidates[0].char_count + candidates[0].next_match_len;

    for (size_t i = 1; i < num_candidates; i++) {
        int curr_pt_score = candidates[i].pt_score;
        size_t total = candidates[i].char_count + candidates[i].next_match_len;

        /* Primary: Portuguese score (higher is better) */
        if (curr_pt_score > best_pt_score) {
            best_idx = i;
            best_pt_score = curr_pt_score;
            best_total = total;
        } else if (curr_pt_score == best_pt_score) {
            /* Same Portuguese score, use coverage as tie-breaker */
            if (total > best_total) {
                best_idx = i;
                best_total = total;
            } else if (total == best_total) {
                /* Same total - apply additional tie-breaking rules */
                int best_at_end = (candidates[best_idx].next_match_len == 0);
                int curr_at_end = (candidates[i].next_match_len == 0);

                if (best_at_end && !curr_at_end) {
                    /* Best reaches end, current doesn't - keep best */
                } else if (!best_at_end && curr_at_end) {
                    /* Current reaches end, best doesn't - prefer current */
                    best_idx = i;
                } else if (best_at_end && curr_at_end) {
                    /* Both at end, prefer longer current (bigger syllable) */
                    if (candidates[i].char_count > candidates[best_idx].char_count) {
                        best_idx = i;
                    }
                } else {
                    /* Neither at end, prefer longer next match */
                    if (candidates[i].next_match_len > candidates[best_idx].next_match_len) {
                        best_idx = i;
                    }
                }
            }
        }
    }

    *out_unit_idx = candidates[best_idx].unit_idx;
    return candidates[best_idx].byte_len;
}

/* Get samples for a unit */
static const int16_t* get_unit_samples(CTTS* engine, int unit_idx, size_t* count) {
    CTTSIndexEntry* entry = &engine->index[unit_idx];
    *count = entry->sample_count;
    return engine->audio + entry->audio_offset;
}

/* ============================================================================
 * Signal Processing
 * ============================================================================ */

/* Remove DC offset from samples */
static void remove_dc_offset(int16_t* samples, size_t count) {
    if (count == 0) return;

    int64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += samples[i];
    }
    int16_t dc = (int16_t)(sum / (int64_t)count);

    for (size_t i = 0; i < count; i++) {
        int32_t val = samples[i] - dc;
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        samples[i] = (int16_t)val;
    }
}

/* Find zero crossing near position (within window) */
static size_t find_zero_crossing(const int16_t* samples, size_t count,
                                  size_t pos, size_t window) {
    if (count == 0) return pos;

    size_t best = pos;
    int16_t best_val = 32767;

    size_t start = (pos > window) ? pos - window : 0;
    size_t end = (pos + window < count) ? pos + window : count - 1;

    for (size_t i = start; i < end; i++) {
        /* Look for sign change (zero crossing) */
        if ((samples[i] <= 0 && samples[i + 1] > 0) ||
            (samples[i] >= 0 && samples[i + 1] < 0)) {
            /* Found a zero crossing */
            int16_t abs_val = samples[i] > 0 ? samples[i] : -samples[i];
            if (abs_val < best_val) {
                best_val = abs_val;
                best = i;
            }
        }
    }

    /* If no zero crossing found, find minimum absolute value */
    if (best == pos) {
        for (size_t i = start; i <= end; i++) {
            int16_t abs_val = samples[i] > 0 ? samples[i] : -samples[i];
            if (abs_val < best_val) {
                best_val = abs_val;
                best = i;
            }
        }
    }

    return best;
}

/* Hanning window value */
static float hanning(size_t i, size_t N) {
    return 0.5f * (1.0f - cosf(2.0f * (float)PI * (float)i / (float)N));
}

/*
 * Remove silence regions from audio buffer.
 * Silence is detected as regions where amplitude is below threshold.
 * Only removes silence longer than min_silence_samples.
 * Returns the new sample count.
 */
static size_t remove_silence_regions(int16_t* samples, size_t count,
                                      float threshold, size_t min_silence_samples) {
    if (count == 0) return 0;

    /* Find max amplitude for relative threshold */
    int16_t max_amp = 0;
    for (size_t i = 0; i < count; i++) {
        int16_t abs_val = samples[i] > 0 ? samples[i] : -samples[i];
        if (abs_val > max_amp) max_amp = abs_val;
    }

    if (max_amp == 0) return count;

    /* Calculate absolute threshold */
    int16_t abs_threshold = (int16_t)(max_amp * threshold);

    /* Find and remove silence regions */
    size_t write_pos = 0;
    size_t read_pos = 0;

    while (read_pos < count) {
        /* Check if current sample is silence */
        int16_t abs_val = samples[read_pos] > 0 ? samples[read_pos] : -samples[read_pos];

        if (abs_val <= abs_threshold) {
            /* Found potential silence - measure its length */
            size_t silence_start = read_pos;
            while (read_pos < count) {
                abs_val = samples[read_pos] > 0 ? samples[read_pos] : -samples[read_pos];
                if (abs_val > abs_threshold) break;
                read_pos++;
            }
            size_t silence_len = read_pos - silence_start;

            if (silence_len >= min_silence_samples) {
                /* Long silence - remove it (keep a tiny bit for smooth transition) */
                size_t keep = min_silence_samples / 4;
                if (keep < 10) keep = 10;

                /* Copy a small portion to avoid hard cut */
                for (size_t i = 0; i < keep && silence_start + i < count; i++) {
                    samples[write_pos++] = samples[silence_start + i];
                }
            } else {
                /* Short silence - keep it */
                for (size_t i = silence_start; i < read_pos; i++) {
                    samples[write_pos++] = samples[i];
                }
            }
        } else {
            /* Not silence - copy sample */
            samples[write_pos++] = samples[read_pos++];
        }
    }

    return write_pos;
}

/* ============================================================================
 * Improved Audio Concatenation
 * ============================================================================ */

typedef struct {
    int16_t* data;
    size_t count;
    size_t capacity;
} SampleBuffer;

static int buffer_init(SampleBuffer* buf, size_t initial_capacity) {
    buf->data = malloc(initial_capacity * sizeof(int16_t));
    if (!buf->data) return CTTS_ERR_OUT_OF_MEMORY;
    buf->count = 0;
    buf->capacity = initial_capacity;
    return CTTS_OK;
}

static int buffer_grow(SampleBuffer* buf, size_t needed) {
    if (buf->count + needed <= buf->capacity) return CTTS_OK;

    size_t new_cap = buf->capacity * 2;
    while (new_cap < buf->count + needed) new_cap *= 2;

    int16_t* new_data = realloc(buf->data, new_cap * sizeof(int16_t));
    if (!new_data) return CTTS_ERR_OUT_OF_MEMORY;

    buf->data = new_data;
    buf->capacity = new_cap;
    return CTTS_OK;
}

/* Apply fade-in to samples (in-place) */
static void apply_fade_in(int16_t* samples, size_t count, size_t fade_samples) {
    if (fade_samples == 0 || count == 0) return;
    if (fade_samples > count) fade_samples = count;

    for (size_t i = 0; i < fade_samples; i++) {
        float gain = (float)i / (float)fade_samples;
        /* Use smooth curve (sine-based) instead of linear */
        gain = sinf(gain * PI * 0.5f);
        samples[i] = (int16_t)(samples[i] * gain);
    }
}

/* Apply fade-out to samples (in-place) */
static void apply_fade_out(int16_t* samples, size_t count, size_t fade_samples) {
    if (fade_samples == 0 || count == 0) return;
    if (fade_samples > count) fade_samples = count;

    size_t start = count - fade_samples;
    for (size_t i = 0; i < fade_samples; i++) {
        float gain = (float)(fade_samples - i) / (float)fade_samples;
        /* Use smooth curve (sine-based) instead of linear */
        gain = sinf(gain * PI * 0.5f);
        samples[start + i] = (int16_t)(samples[start + i] * gain);
    }
}

/* Check if a character is a vowel (including Portuguese accented vowels) */
static int is_vowel(uint32_t cp) {
    /* Basic vowels */
    if (cp == 'a' || cp == 'e' || cp == 'i' || cp == 'o' || cp == 'u' ||
        cp == 'A' || cp == 'E' || cp == 'I' || cp == 'O' || cp == 'U') {
        return 1;
    }
    /* Portuguese accented vowels */
    if (cp == 0xE1 || cp == 0xC1 ||   /* á Á */
        cp == 0xE0 || cp == 0xC0 ||   /* à À */
        cp == 0xE2 || cp == 0xC2 ||   /* â Â */
        cp == 0xE3 || cp == 0xC3 ||   /* ã Ã */
        cp == 0xE9 || cp == 0xC9 ||   /* é É */
        cp == 0xEA || cp == 0xCA ||   /* ê Ê */
        cp == 0xED || cp == 0xCD ||   /* í Í */
        cp == 0xF3 || cp == 0xD3 ||   /* ó Ó */
        cp == 0xF4 || cp == 0xD4 ||   /* ô Ô */
        cp == 0xF5 || cp == 0xD5 ||   /* õ Õ */
        cp == 0xFA || cp == 0xDA ||   /* ú Ú */
        cp == 0xFC || cp == 0xDC) {   /* ü Ü */
        return 1;
    }
    return 0;
}

/* Check if text ends with a vowel */
static int ends_with_vowel(const char* text, size_t len) {
    if (len == 0) return 0;

    /* Find last character */
    const char* p = text;
    const char* last_char = text;
    while (p < text + len) {
        last_char = p;
        p += utf8_char_len(p);
    }

    /* Decode last character */
    uint32_t cp = ctts_utf8_next(&last_char);
    return is_vowel(cp);
}

/* Check if text ends with 'S' or 's' */
static int ends_with_s(const char* text, size_t len) {
    if (len == 0) return 0;

    /* Find last character */
    const char* p = text;
    const char* last_char = text;
    while (p < text + len) {
        last_char = p;
        p += utf8_char_len(p);
    }

    /* Decode last character */
    uint32_t cp = ctts_utf8_next(&last_char);
    return (cp == 's' || cp == 'S');
}

/* Check if text ends with 'R' or 'r' */
static int ends_with_r(const char* text, size_t len) {
    if (len == 0) return 0;

    /* Find last character */
    const char* p = text;
    const char* last_char = text;
    while (p < text + len) {
        last_char = p;
        p += utf8_char_len(p);
    }

    /* Decode last character */
    uint32_t cp = ctts_utf8_next(&last_char);
    return (cp == 'r' || cp == 'R');
}

/* Check if text starts with a consonant */
static int starts_with_consonant(const char* text, size_t len) {
    if (len == 0) return 0;

    /* Get first character */
    uint32_t cp = ctts_utf8_next(&text);

    /* A consonant is a letter that's not a vowel */
    /* Check if it's a letter first (basic ASCII + common Portuguese) */
    int is_letter = (cp >= 'a' && cp <= 'z') ||
                    (cp >= 'A' && cp <= 'Z') ||
                    (cp == 0xE7 || cp == 0xC7);  /* ç Ç */

    return is_letter && !is_vowel(cp);
}

/* ============================================================================
 * Portuguese Pronunciation Rules
 * ============================================================================ */

/* Check if a character (lowercase) is a Portuguese consonant */
static int is_pt_consonant(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') cp = cp + 32;  /* lowercase */
    if (cp == 0xC7) cp = 0xE7;  /* Ç -> ç */

    return (cp >= 'a' && cp <= 'z' && !is_vowel(cp)) || cp == 0xE7;
}

/* Check if text is a Portuguese digraph that should stay together */
static int is_pt_digraph(const char* text, size_t len) {
    if (len < 2) return 0;

    char c1 = text[0];
    char c2 = text[1];

    /* Lowercase for comparison */
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;

    /* Indivisible digraphs: ch, lh, nh, qu, gu */
    if (c1 == 'c' && c2 == 'h') return 1;
    if (c1 == 'l' && c2 == 'h') return 1;
    if (c1 == 'n' && c2 == 'h') return 1;
    if (c1 == 'q' && c2 == 'u') return 1;
    if (c1 == 'g' && c2 == 'u') return 1;

    return 0;
}

/* Check if text starts with a valid Portuguese consonant cluster */
static int is_pt_valid_cluster(const char* text, size_t len) {
    if (len < 2) return 0;

    char c1 = text[0];
    char c2 = text[1];

    /* Lowercase for comparison */
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;

    /* Valid onset clusters: obstruent + liquid (r or l) */
    /* With r: pr, br, tr, dr, cr, gr, fr, vr */
    if (c2 == 'r') {
        return (c1 == 'p' || c1 == 'b' || c1 == 't' || c1 == 'd' ||
                c1 == 'c' || c1 == 'g' || c1 == 'f' || c1 == 'v');
    }

    /* With l: pl, bl, cl, gl, fl */
    if (c2 == 'l') {
        return (c1 == 'p' || c1 == 'b' || c1 == 'c' || c1 == 'g' || c1 == 'f');
    }

    return 0;
}

/* Check if a single-char match should be rejected based on Portuguese rules */
static int pt_reject_single_consonant(const char* pos, size_t match_char_count,
                                       int at_word_start) {
    if (match_char_count != 1) return 0;  /* Only check single-char matches */

    /* Get the character */
    const char* p = pos;
    uint32_t cp = ctts_utf8_next(&p);

    /* If it's a vowel, single char is OK */
    if (is_vowel(cp)) return 0;

    /* Single consonant at word start: REJECT (need at least CV) */
    if (at_word_start) return 1;

    /* Single consonant NOT at word start: check if it breaks a digraph */
    /* If the next char forms a digraph with this one, reject */
    if (*p) {
        char test[3] = {0};
        test[0] = (cp >= 'A' && cp <= 'Z') ? (cp + 32) : cp;
        test[1] = (*p >= 'A' && *p <= 'Z') ? (*p + 32) : *p;
        if (is_pt_digraph(test, 2)) return 1;
    }

    return 0;
}

/* Calculate Portuguese syllable quality score for a match */
static int pt_syllable_score(const char* text, size_t byte_len, size_t char_count,
                              int at_word_start) {
    int score = (int)char_count * 10;  /* Base score: prefer longer matches */

    if (char_count == 0) return -1000;

    /* Get first character */
    const char* p = text;
    uint32_t first_cp = ctts_utf8_next(&p);
    int first_is_consonant = is_pt_consonant(first_cp);

    /* Bonus for starting with valid Portuguese patterns */
    if (char_count >= 2) {
        if (is_pt_digraph(text, byte_len)) {
            score += 20;  /* Digraph bonus */
        }
        if (first_is_consonant && is_pt_valid_cluster(text, byte_len)) {
            score += 15;  /* Valid cluster bonus */
        }
    }

    /* At word start: strongly prefer CV or CCV patterns over single C */
    if (at_word_start && first_is_consonant) {
        if (char_count == 1) {
            score -= 100;  /* Heavy penalty for single consonant at word start */
        } else {
            /* Check if second char is vowel (CV pattern) */
            if (*p) {
                uint32_t second_cp = ctts_utf8_next(&p);
                if (is_vowel(second_cp)) {
                    score += 25;  /* CV pattern bonus */
                }
            }
        }
    }

    /* Bonus for ending with vowel (open syllable - common in Portuguese) */
    const char* end_p = text;
    uint32_t last_cp = 0;
    while (end_p < text + byte_len) {
        uint32_t cp = ctts_utf8_next(&end_p);
        last_cp = cp;  /* Keep track of last character */
    }
    if (is_vowel(last_cp)) {
        score += 10;  /* Open syllable bonus */
    }

    return score;
}

/*
 * Append audio unit with crossfade.
 * Uses smooth crossfade for syllable concatenation.
 * Does NOT truncate audio - keeps full samples with fade in/out.
 * crossfade_ms parameter allows caller to specify crossfade duration.
 * after_word_boundary: if true, apply fade-in instead of crossfade (first unit of word)
 */
static int buffer_append_crossfade(SampleBuffer* buf, const int16_t* samples,
                                    size_t count, float crossfade_ms,
                                    const CTTSConfig* config,
                                    int after_word_boundary) {
    if (count == 0) return CTTS_OK;

    size_t crossfade_samples = (size_t)(crossfade_ms * CTTS_SAMPLE_RATE / 1000.0f);
    size_t fade_in_samples = (size_t)(config->fade_in_ms * CTTS_SAMPLE_RATE / 1000.0f);

    /* Make a copy to process */
    int16_t* copy = malloc(count * sizeof(int16_t));
    if (!copy) return CTTS_ERR_OUT_OF_MEMORY;
    memcpy(copy, samples, count * sizeof(int16_t));

    /* Remove DC offset if configured */
    if (config->remove_dc_offset) {
        remove_dc_offset(copy, count);
    }

    int err = buffer_grow(buf, count + crossfade_samples);
    if (err != CTTS_OK) {
        free(copy);
        return err;
    }

    if (buf->count == 0 || after_word_boundary) {
        /* First segment or first unit after word boundary - apply fade-in at start */
        apply_fade_in(copy, count, fade_in_samples);
        memcpy(buf->data + buf->count, copy, count * sizeof(int16_t));
        buf->count += count;
    } else if (crossfade_samples == 0) {
        /* No crossfade - just append */
        memcpy(buf->data + buf->count, copy, count * sizeof(int16_t));
        buf->count += count;
    } else {
        /* Crossfade with previous audio (within a word) */
        size_t actual_crossfade = crossfade_samples;
        if (actual_crossfade > buf->count) actual_crossfade = buf->count;
        if (actual_crossfade > count) actual_crossfade = count;

        /* Crossfade region */
        if (actual_crossfade > 0) {
            size_t fade_start = buf->count - actual_crossfade;
            for (size_t i = 0; i < actual_crossfade; i++) {
                /* Smooth crossfade using raised cosine */
                float t = (float)i / (float)actual_crossfade;
                float prev_gain = 0.5f * (1.0f + cosf(PI * t));  /* 1 -> 0 */
                float next_gain = 0.5f * (1.0f - cosf(PI * t));  /* 0 -> 1 */

                int32_t prev_sample = buf->data[fade_start + i];
                int32_t next_sample = copy[i];

                int32_t mixed = (int32_t)(prev_sample * prev_gain + next_sample * next_gain);

                if (mixed > 32767) mixed = 32767;
                if (mixed < -32768) mixed = -32768;

                buf->data[fade_start + i] = (int16_t)mixed;
            }
        }

        /* Append the rest of the new samples (after crossfade region) */
        if (count > actual_crossfade) {
            memcpy(buf->data + buf->count,
                   copy + actual_crossfade,
                   (count - actual_crossfade) * sizeof(int16_t));
            buf->count += count - actual_crossfade;
        }
    }

    free(copy);
    return CTTS_OK;
}

/* Append pure silence (for word boundaries) */
static int buffer_append_silence(SampleBuffer* buf, size_t samples) {
    int err = buffer_grow(buf, samples);
    if (err != CTTS_OK) return err;

    memset(buf->data + buf->count, 0, samples * sizeof(int16_t));
    buf->count += samples;
    return CTTS_OK;
}

/* Apply final fade-out to buffer end */
static void buffer_finalize(SampleBuffer* buf, size_t fade_out_samples) {
    if (buf->count > 0 && fade_out_samples > 0) {
        apply_fade_out(buf->data, buf->count, fade_out_samples);
    }
}

/* ============================================================================
 * Time Stretching (PSOLA-like)
 * ============================================================================ */

static int time_stretch(const int16_t* input, size_t input_count,
                        int16_t** output, size_t* output_count,
                        float speed_factor) {
    if (speed_factor < CTTS_MIN_SPEED) speed_factor = CTTS_MIN_SPEED;
    if (speed_factor > CTTS_MAX_SPEED) speed_factor = CTTS_MAX_SPEED;

    /* Frame parameters */
    const size_t frame_size = 441;  /* 20ms at 22050 Hz */
    const size_t analysis_hop = frame_size / 4;
    size_t synthesis_hop = (size_t)(analysis_hop / speed_factor);

    /* Calculate output size */
    size_t num_frames = (input_count - frame_size) / analysis_hop + 1;
    *output_count = num_frames * synthesis_hop + frame_size;

    *output = calloc(*output_count, sizeof(int16_t));
    if (!*output) return CTTS_ERR_OUT_OF_MEMORY;

    /* Precompute window */
    float* window = malloc(frame_size * sizeof(float));
    if (!window) {
        free(*output);
        return CTTS_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < frame_size; i++) {
        window[i] = hanning(i, frame_size);
    }

    /* Accumulator for normalization */
    float* norm = calloc(*output_count, sizeof(float));
    if (!norm) {
        free(*output);
        free(window);
        return CTTS_ERR_OUT_OF_MEMORY;
    }

    /* Process frames */
    size_t analysis_pos = 0;
    size_t synthesis_pos = 0;

    while (analysis_pos + frame_size <= input_count &&
           synthesis_pos + frame_size <= *output_count) {
        /* Overlap-add with window */
        for (size_t i = 0; i < frame_size; i++) {
            float sample = input[analysis_pos + i] * window[i];
            (*output)[synthesis_pos + i] += (int16_t)sample;
            norm[synthesis_pos + i] += window[i];
        }

        analysis_pos += analysis_hop;
        synthesis_pos += synthesis_hop;
    }

    /* Normalize */
    for (size_t i = 0; i < *output_count; i++) {
        if (norm[i] > 0.01f) {
            float val = (*output)[i] / norm[i];
            if (val > 32767) val = 32767;
            if (val < -32768) val = -32768;
            (*output)[i] = (int16_t)val;
        }
    }

    free(window);
    free(norm);

    /* Trim trailing silence */
    while (*output_count > 0 && (*output)[*output_count - 1] == 0) {
        (*output_count)--;
    }

    return CTTS_OK;
}

/* ============================================================================
 * Text-to-Speech Synthesis
 * ============================================================================ */

int ctts_synthesize(CTTS* engine, const char* text,
                    int16_t** samples, size_t* sample_count, float speed) {
    if (!engine || !text || !samples || !sample_count) {
        return CTTS_ERR_INVALID_ARG;
    }

    /* Get config */
    CTTSConfig* config = &engine->config;

    /* Load normalization rules from CSV (once) */
    ctts_load_normalization("normalization.csv");

    /* Apply CSV normalization rules first (regex replacements) */
    char* rule_normalized = ctts_apply_normalization(text);
    if (!rule_normalized) return CTTS_ERR_OUT_OF_MEMORY;

    /* Then apply standard normalization (lowercase) */
    char* normalized = ctts_normalize(rule_normalized);
    free(rule_normalized);
    if (!normalized) return CTTS_ERR_OUT_OF_MEMORY;

    /* Initialize output buffer */
    SampleBuffer buf;
    int err = buffer_init(&buf, CTTS_SAMPLE_RATE * 10);  /* 10 seconds initial */
    if (err != CTTS_OK) {
        free(normalized);
        return err;
    }

    /* Calculate sample counts from config */
    size_t word_pause_samples = (size_t)(config->word_pause_ms * CTTS_SAMPLE_RATE / 1000.0f);
    size_t unknown_silence = (size_t)(config->unknown_silence_ms * CTTS_SAMPLE_RATE / 1000.0f);

    const char* pos = normalized;
    engine->units_found = 0;
    engine->units_missing = 0;

    /* Track previous unit for vowel detection */
    const char* prev_unit_text = NULL;
    size_t prev_unit_len = 0;
    int prev_was_word_boundary = 1;  /* Start as if after word boundary */

    /* Track word start position for silence removal within words */
    size_t word_start_sample = 0;

    /* Silence removal parameters */
    size_t min_silence_samples = (size_t)(config->min_silence_ms * CTTS_SAMPLE_RATE / 1000.0f);

    while (*pos) {
        /* Skip whitespace, add word pause (pure silence, no crossfade) */
        if (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') {
            /* Remove silence within the completed word if configured */
            if (config->remove_word_silence && buf.count > word_start_sample) {
                size_t word_samples = buf.count - word_start_sample;
                if (word_samples > min_silence_samples) {
                    size_t new_word_len = remove_silence_regions(
                        buf.data + word_start_sample,
                        word_samples,
                        config->silence_threshold,
                        min_silence_samples
                    );
                    buf.count = word_start_sample + new_word_len;
                }
            }

            /* Apply fade-out before silence if we have audio */
            if (buf.count > 0) {
                size_t fade_samples = (size_t)(config->fade_out_ms * CTTS_SAMPLE_RATE / 1000.0f);
                apply_fade_out(buf.data, buf.count, fade_samples);
            }
            buffer_append_silence(&buf, word_pause_samples);

            /* Mark start of next word */
            word_start_sample = buf.count;

            pos++;
            prev_was_word_boundary = 1;
            prev_unit_text = NULL;
            prev_unit_len = 0;
            continue;
        }

        /* Soft syllable separator: hyphen creates smooth transition without pause */
        /* Used for hiatus and other cases where vowels should flow together */
        if (*pos == '-') {
            pos++;
            /* Don't reset prev_was_word_boundary - allow smooth crossfade to continue */
            /* Don't add any silence - just skip the separator */
            continue;
        }

        /* Use greedy matching with look-ahead and Portuguese rules */
        int unit_idx;
        size_t match_len = find_best_match_with_lookahead(
            engine, pos, engine->header.max_unit_chars, &unit_idx, prev_was_word_boundary);

        if (match_len > 0 && unit_idx >= 0) {
            /* Found a match */
            size_t unit_samples;
            const int16_t* unit_audio = get_unit_samples(engine, unit_idx, &unit_samples);

            /* Get unit text for vowel detection */
            CTTSIndexEntry* entry = &engine->index[unit_idx];
            const char* unit_text = engine->strings + entry->string_offset;

            /* Debug output if enabled */
            if (config->print_units) {
                fprintf(stderr, "  [%.*s] ", (int)entry->string_len, unit_text);
            }

            /* Choose crossfade duration based on previous/current unit transitions */
            float crossfade_ms = config->crossfade_ms;
            if (!prev_was_word_boundary && prev_unit_text != NULL) {
                int prev_ends_vowel = ends_with_vowel(prev_unit_text, prev_unit_len);
                int prev_ends_s = ends_with_s(prev_unit_text, prev_unit_len);
                int prev_ends_r = ends_with_r(prev_unit_text, prev_unit_len);
                int curr_starts_consonant = starts_with_consonant(unit_text, entry->string_len);

                if (prev_ends_s) {
                    /* S-ending: use shorter crossfade for crisp S sound */
                    crossfade_ms = config->crossfade_s_ending_ms;
                } else if (prev_ends_r) {
                    /* R-ending: use shorter crossfade for crisp R sound */
                    crossfade_ms = config->crossfade_r_ending_ms;
                } else if (prev_ends_vowel && curr_starts_consonant) {
                    /* Vowel-to-consonant: use shorter crossfade (crisp transition) */
                    crossfade_ms = config->crossfade_ms * config->vowel_to_consonant_factor;
                } else if (prev_ends_vowel) {
                    /* Vowel-to-vowel: use longer crossfade (smooth blend) */
                    crossfade_ms = config->crossfade_vowel_ms;
                }
            }

            /* Append with appropriate crossfade (or fade-in if first unit of word) */
            err = buffer_append_crossfade(&buf, unit_audio, unit_samples, crossfade_ms,
                                          config, prev_was_word_boundary);
            if (err != CTTS_OK) {
                free(normalized);
                free(buf.data);
                return err;
            }

            /* Update previous unit tracking */
            prev_unit_text = unit_text;
            prev_unit_len = entry->string_len;
            prev_was_word_boundary = 0;

            pos += match_len;
            engine->units_found++;
        } else {
            /* No match found, add silence and skip character */
            buffer_append_silence(&buf, unknown_silence);
            pos += utf8_char_len(pos);
            engine->units_missing++;
            prev_unit_text = NULL;
            prev_unit_len = 0;
        }
    }

    if (config->print_units) {
        fprintf(stderr, "\n");
    }

    /* Remove silence from the last word (if not followed by whitespace) */
    if (config->remove_word_silence && buf.count > word_start_sample) {
        size_t word_samples = buf.count - word_start_sample;
        if (word_samples > min_silence_samples) {
            size_t new_word_len = remove_silence_regions(
                buf.data + word_start_sample,
                word_samples,
                config->silence_threshold,
                min_silence_samples
            );
            buf.count = word_start_sample + new_word_len;
        }
    }

    free(normalized);

    /* Apply final fade-out */
    size_t final_fade = (size_t)(config->fade_out_ms * CTTS_SAMPLE_RATE / 1000.0f);
    buffer_finalize(&buf, final_fade);

    /* Apply time stretching if needed */
    if (speed != 1.0f) {
        int16_t* stretched;
        size_t stretched_count;
        err = time_stretch(buf.data, buf.count, &stretched, &stretched_count, speed);
        if (err != CTTS_OK) {
            free(buf.data);
            return err;
        }
        free(buf.data);
        *samples = stretched;
        *sample_count = stretched_count;
    } else {
        *samples = buf.data;
        *sample_count = buf.count;
    }

    return CTTS_OK;
}

/* ============================================================================
 * Main Program (Command Line Interface)
 * ============================================================================ */

static void print_usage(const char* progname) {
    fprintf(stderr, "CTTS - Concatenative Text-to-Speech Engine\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  Build database:\n");
    fprintf(stderr, "    %s build <dataset_dir> <output.db>\n\n", progname);
    fprintf(stderr, "  Synthesize speech:\n");
    fprintf(stderr, "    %s synth <database.db> \"text\" <output.wav> [speed]\n\n", progname);
    fprintf(stderr, "  Options:\n");
    fprintf(stderr, "    speed  - Playback speed (0.5 to 2.0, default 1.0)\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "build") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s build <dataset_dir> <output.db>\n", argv[0]);
            return 1;
        }

        char letters_dir[1024], letters_index[1024];
        char syllables_dir[1024], syllables_index[1024];

        snprintf(letters_dir, sizeof(letters_dir), "%s/letters/wavs", argv[2]);
        snprintf(letters_index, sizeof(letters_index), "%s/letters/letters.txt", argv[2]);
        snprintf(syllables_dir, sizeof(syllables_dir), "%s/syllables/wavs", argv[2]);
        snprintf(syllables_index, sizeof(syllables_index), "%s/syllables/sillabes.txt", argv[2]);

        int err = ctts_build_database(letters_dir, letters_index,
                                      syllables_dir, syllables_index,
                                      argv[3]);
        if (err != CTTS_OK) {
            fprintf(stderr, "Build failed: %s\n", ctts_strerror(err));
            return 1;
        }
        return 0;

    } else if (strcmp(argv[1], "synth") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s synth <database.db> \"text\" <output.wav> [speed]\n", argv[0]);
            return 1;
        }

        float speed = 1.0f;
        if (argc > 5) {
            speed = strtof(argv[5], NULL);
            if (speed < CTTS_MIN_SPEED) speed = CTTS_MIN_SPEED;
            if (speed > CTTS_MAX_SPEED) speed = CTTS_MAX_SPEED;
        }

        CTTS* engine = ctts_init(argv[2]);
        if (!engine) {
            fprintf(stderr, "Failed to load database: %s\n", argv[2]);
            return 1;
        }

        /* Load config from config.yaml if present */
        ctts_load_config(&engine->config, "config.yaml");

        /* Override speed from config if not specified on command line */
        if (argc <= 5 && engine->config.default_speed != 1.0f) {
            speed = engine->config.default_speed;
        }

        printf("Loaded database with %u units\n", engine->header.unit_count);
        printf("Config: crossfade=%.1fms (vowel=%.1fms, v2c=%.0f%%), word_pause=%.1fms\n",
               engine->config.crossfade_ms, engine->config.crossfade_vowel_ms,
               engine->config.vowel_to_consonant_factor * 100,
               engine->config.word_pause_ms);

        int16_t* samples;
        size_t sample_count;
        int err = ctts_synthesize(engine, argv[3], &samples, &sample_count, speed);
        if (err != CTTS_OK) {
            fprintf(stderr, "Synthesis failed: %s\n", ctts_strerror(err));
            ctts_free(engine);
            return 1;
        }

        printf("Synthesized %zu samples (%.2f seconds)\n",
               sample_count, (float)sample_count / CTTS_SAMPLE_RATE);
        printf("Units found: %u, missing: %u\n",
               engine->units_found, engine->units_missing);

        err = ctts_write_wav(argv[4], samples, sample_count, CTTS_SAMPLE_RATE);
        if (err != CTTS_OK) {
            fprintf(stderr, "Failed to write WAV: %s\n", ctts_strerror(err));
            ctts_free_samples(samples);
            ctts_free(engine);
            return 1;
        }

        printf("Written to %s\n", argv[4]);

        ctts_free_samples(samples);
        ctts_free(engine);
        return 0;

    } else {
        print_usage(argv[0]);
        return 1;
    }
}
