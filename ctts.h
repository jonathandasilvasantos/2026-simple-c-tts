/*
 * CTTS - Concatenative Text-to-Speech Engine
 * Pure C implementation without external dependencies
 *
 * Copyright (c) 2024
 */

#ifndef CTTS_H
#define CTTS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CTTS_MAGIC          0x53545443  /* "CTTS" in little-endian */
#define CTTS_VERSION        1
#define CTTS_SAMPLE_RATE    22050
#define CTTS_BITS_PER_SAMPLE 16
#define CTTS_MAX_UNIT_LEN   16          /* Maximum characters per unit */

/* Default parameters */
#define CTTS_DEFAULT_CROSSFADE_MS       20.0f
#define CTTS_DEFAULT_WORD_PAUSE_MS      120.0f
#define CTTS_DEFAULT_UNKNOWN_SILENCE_MS 30.0f
#define CTTS_DEFAULT_FADE_IN_MS         3.0f
#define CTTS_DEFAULT_FADE_OUT_MS        3.0f
#define CTTS_DEFAULT_SPEED              1.0f

/* Speed limits */
#define CTTS_MIN_SPEED      0.5f
#define CTTS_MAX_SPEED      2.0f

/* ============================================================================
 * Configuration Structure
 * ============================================================================ */

typedef struct {
    /* Audio concatenation */
    float crossfade_ms;         /* Crossfade between syllables */
    float crossfade_vowel_ms;   /* Crossfade when prev syllable ends with vowel */
    float crossfade_s_ending_ms; /* Crossfade when prev syllable ends with S */
    float crossfade_r_ending_ms; /* Crossfade when prev syllable ends with R */
    float vowel_to_consonant_factor;  /* Multiplier for vowel-to-consonant transitions */
    float word_pause_ms;        /* Silence between words */
    float unknown_silence_ms;   /* Silence for unknown chars */
    float fade_in_ms;           /* Fade-in at unit start */
    float fade_out_ms;          /* Fade-out at unit end */

    /* Silence removal within words */
    int remove_word_silence;    /* Remove silence within words */
    float silence_threshold;    /* Threshold for silence detection (0-1) */
    float min_silence_ms;       /* Minimum silence duration to detect */

    /* Processing */
    int remove_dc_offset;       /* Remove DC offset */
    float normalize_level;      /* Normalize level (0=disabled) */
    float compression;          /* Compression amount (0=disabled) */

    /* Synthesis */
    float default_speed;
    float min_speed;
    float max_speed;

    /* Debug */
    int print_units;
    int print_timing;
} CTTSConfig;

/* ============================================================================
 * Database Structures (on-disk format)
 * ============================================================================ */

/* Database header - 64 bytes */
typedef struct {
    uint32_t magic;             /* CTTS_MAGIC */
    uint32_t version;           /* CTTS_VERSION */
    uint32_t unit_count;        /* Number of units */
    uint32_t sample_rate;       /* Audio sample rate */
    uint32_t bits_per_sample;   /* Bits per sample (16) */
    uint32_t index_offset;      /* Offset to index table */
    uint32_t strings_offset;    /* Offset to string pool */
    uint32_t audio_offset;      /* Offset to audio data */
    uint32_t total_samples;     /* Total audio samples */
    uint32_t max_unit_chars;    /* Maximum unit length in characters */
    uint32_t hash_table_size;   /* Hash table size for lookups */
    uint32_t hash_table_offset; /* Offset to hash table */
    uint8_t  reserved[16];      /* Reserved for future use */
} CTTSHeader;

/* Index entry - 32 bytes per unit */
typedef struct {
    uint32_t hash;              /* FNV-1a hash of text */
    uint32_t string_offset;     /* Offset into string pool */
    uint16_t string_len;        /* String length in bytes */
    uint16_t char_count;        /* Character count (UTF-8 aware) */
    uint32_t audio_offset;      /* Offset into audio data (in samples) */
    uint32_t sample_count;      /* Number of samples */
    uint32_t flags;             /* Reserved flags */
    uint32_t next_hash;         /* Next entry with same hash (chaining) */
    uint32_t reserved;          /* Reserved */
} CTTSIndexEntry;

/* ============================================================================
 * Runtime Structures
 * ============================================================================ */

/* Unit descriptor (in-memory) */
typedef struct {
    char* text;                 /* Unit text (UTF-8) */
    uint16_t text_len;          /* Text length in bytes */
    uint16_t char_count;        /* Character count */
    int16_t* samples;           /* Audio samples */
    uint32_t sample_count;      /* Number of samples */
    uint32_t hash;              /* Precomputed hash */
} CTTSUnit;

/* Main engine structure */
typedef struct {
    /* Database mapping */
    uint8_t* db_data;           /* Memory-mapped database */
    size_t db_size;             /* Database size */
    int db_fd;                  /* File descriptor (for munmap) */

    /* Parsed header */
    CTTSHeader header;

    /* Pointers into mapped data */
    CTTSIndexEntry* index;      /* Index table */
    uint32_t* hash_table;       /* Hash table for O(1) lookup */
    char* strings;              /* String pool */
    int16_t* audio;             /* Audio data */

    /* Configuration */
    CTTSConfig config;          /* All configuration parameters */

    /* Statistics */
    uint32_t units_found;       /* Units successfully matched */
    uint32_t units_missing;     /* Units not found (fallback) */
} CTTS;

/* Synthesis result */
typedef struct {
    int16_t* samples;           /* Output samples (caller must free) */
    size_t sample_count;        /* Number of samples */
    size_t capacity;            /* Allocated capacity */
} CTTSSynthResult;

/* ============================================================================
 * Database Building API
 * ============================================================================ */

/*
 * Build a database from WAV files
 *
 * Parameters:
 *   letters_dir     - Directory containing letter WAV files
 *   letters_index   - Index file for letters (filename|text|display)
 *   syllables_dir   - Directory containing syllable WAV files
 *   syllables_index - Index file for syllables
 *   output_file     - Output database file path
 *
 * Returns:
 *   0 on success, negative error code on failure
 */
int ctts_build_database(
    const char* letters_dir,
    const char* letters_index,
    const char* syllables_dir,
    const char* syllables_index,
    const char* output_file
);

/* ============================================================================
 * Synthesis API
 * ============================================================================ */

/*
 * Initialize TTS engine with a database file
 *
 * Parameters:
 *   database_file - Path to compiled database
 *
 * Returns:
 *   Pointer to engine on success, NULL on failure
 */
CTTS* ctts_init(const char* database_file);

/*
 * Synthesize text to audio samples
 *
 * Parameters:
 *   engine       - Initialized engine
 *   text         - Input text (UTF-8)
 *   samples      - Output: pointer to allocated sample buffer
 *   sample_count - Output: number of samples
 *   speed        - Speed factor (0.5 to 2.0, 1.0 = normal)
 *
 * Returns:
 *   0 on success, negative error code on failure
 *   Caller must free *samples with ctts_free_samples()
 */
int ctts_synthesize(
    CTTS* engine,
    const char* text,
    int16_t** samples,
    size_t* sample_count,
    float speed
);

/*
 * Write samples to WAV file
 *
 * Parameters:
 *   filename     - Output WAV file path
 *   samples      - Audio samples
 *   sample_count - Number of samples
 *   sample_rate  - Sample rate (usually CTTS_SAMPLE_RATE)
 *
 * Returns:
 *   0 on success, negative error code on failure
 */
int ctts_write_wav(
    const char* filename,
    const int16_t* samples,
    size_t sample_count,
    int sample_rate
);

/*
 * Free engine resources
 */
void ctts_free(CTTS* engine);

/*
 * Free sample buffer
 */
void ctts_free_samples(int16_t* samples);

/* ============================================================================
 * Configuration API
 * ============================================================================ */

/*
 * Load configuration from YAML file
 * Returns 0 on success, negative on error
 * If file doesn't exist, uses defaults
 */
int ctts_load_config(CTTSConfig* config, const char* config_file);

/*
 * Initialize config with default values
 */
void ctts_config_defaults(CTTSConfig* config);

/*
 * Set crossfade duration for concatenation (in milliseconds)
 */
void ctts_set_crossfade(CTTS* engine, float crossfade_ms);

/*
 * Set pause duration between words (in milliseconds)
 */
void ctts_set_word_pause(CTTS* engine, float pause_ms);

/*
 * Set silence duration for unknown characters (in milliseconds)
 */
void ctts_set_unknown_silence(CTTS* engine, float silence_ms);

/*
 * Set fade-in/fade-out durations (in milliseconds)
 */
void ctts_set_fades(CTTS* engine, float fade_in_ms, float fade_out_ms);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/*
 * Count UTF-8 characters in a string
 */
size_t ctts_utf8_strlen(const char* str);

/*
 * Get next UTF-8 character and advance pointer
 * Returns the Unicode codepoint, advances *str
 */
uint32_t ctts_utf8_next(const char** str);

/*
 * Compute FNV-1a hash of a string
 */
uint32_t ctts_hash(const char* str, size_t len);

/*
 * Normalize text for lookup (lowercase, etc.)
 * Returns allocated string that caller must free
 */
char* ctts_normalize(const char* text);

/*
 * Load normalization rules from CSV file
 * Format: regex_pattern,replacement (one per line, no header)
 * Returns 0 on success
 */
int ctts_load_normalization(const char* csv_file);

/*
 * Apply normalization rules to text
 * Returns allocated string that caller must free
 */
char* ctts_apply_normalization(const char* text);

/*
 * Free normalization rules
 */
void ctts_free_normalization(void);

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define CTTS_OK                  0
#define CTTS_ERR_INVALID_ARG    -1
#define CTTS_ERR_FILE_NOT_FOUND -2
#define CTTS_ERR_FILE_READ      -3
#define CTTS_ERR_FILE_WRITE     -4
#define CTTS_ERR_INVALID_FORMAT -5
#define CTTS_ERR_OUT_OF_MEMORY  -6
#define CTTS_ERR_INVALID_WAV    -7
#define CTTS_ERR_VERSION        -8

/*
 * Get error message for error code
 */
const char* ctts_strerror(int error_code);

#ifdef __cplusplus
}
#endif

#endif /* CTTS_H */
