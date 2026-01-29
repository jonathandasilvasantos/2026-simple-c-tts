# CTTS - Concatenative Text-to-Speech Engine

A lightweight, pure C implementation of a concatenative Text-to-Speech (TTS) engine designed for Brazilian Portuguese. No external dependencies required - just GCC and the standard library.

## Features

- **Pure C implementation** - No external libraries needed
- **Concatenative synthesis** - Uses pre-recorded speech units (letters, syllables) for natural sound
- **Portuguese phonological rules** - Intelligent syllable selection following Brazilian Portuguese patterns
- **Configurable audio processing** - YAML-based configuration for fine-tuning
- **Smooth concatenation** - Raised-cosine crossfade between units
- **Speed control** - PSOLA-based time stretching (0.5x to 2.0x) without pitch distortion
- **Text normalization rules** - CSV-based regex rules for pronunciation customization

## How It Works

The engine uses **unit selection synthesis**, where pre-recorded speech units are concatenated to form continuous speech:

1. **Text normalization** - Applies regex rules from `normalization.csv`, then lowercase conversion
2. **Unit selection** - Greedy longest-match algorithm with look-ahead selects optimal syllables
3. **Portuguese rules** - Applies phonological rules (digraphs, valid clusters, CV patterns)
4. **Audio assembly** - Units are concatenated with smooth crossfade
5. **Time stretching** - Optional PSOLA processing for speed adjustment

### Portuguese Phonological Rules

The engine implements Brazilian Portuguese pronunciation rules:

- **No single consonant at word start** - Requires CV (consonant-vowel) minimum
- **Digraph preservation** - Keeps ch, lh, nh, qu, gu together
- **Valid consonant clusters** - Recognizes pr, br, tr, dr, cr, gr, fr, pl, bl, cl, gl, fl
- **Open syllable preference** - Favors syllables ending in vowels

## Pronunciation Normalization

The `normalization.csv` file allows you to define custom pronunciation rules using regular expressions. Rules are applied before synthesis to transform input text.

### Format

```csv
# Comments start with #
pattern,replacement
```

### Features

- **POSIX Extended Regex** - Full regex support for patterns
- **Word boundaries** - Use `\b` for portable word boundary matching
- **Backreferences** - Use `\1`, `\2`, etc. in replacements to preserve captured groups

### Example Rules

```csv
# Words starting with 'r' get double 'r' sound
\br,rr

# 's' between vowels becomes 'z' sound (Brazilian Portuguese)
([a-z])sa,\1za
([a-z])se,\1ze
([a-z])si,\1zi
([a-z])so,\1zo
([a-z])su,\1zu

# Word-final 't' gets 'i' sound
t\b,ti

# Specific word replacements
música,muzica
brasil,brazil
```

### How It Works

| Input | Rule Applied | Output |
|-------|--------------|--------|
| rosa | `\br,rr` | rrosa |
| casa | `([a-z])sa,\1za` | caza |
| preciso | `([a-z])so,\1zo` | precizo |
| internet | `t\b,ti` | interneti |

## Building

```bash
make
```

## Usage

### Build Voice Database

First, build a voice database from your dataset:

```bash
./ctts build ./dataset voice.db
```

The dataset should have this structure:
```
dataset/
├── letters/
│   ├── letters.txt      # Index: filename|text|display
│   └── wavs/            # Letter WAV files
└── syllables/
    ├── sillabes.txt     # Index: filename|text|display
    └── wavs/            # Syllable WAV files
```

### Synthesize Speech

```bash
./ctts synth voice.db "olá mundo" output.wav
```

With speed adjustment:
```bash
./ctts synth voice.db "olá mundo" output.wav 1.5  # 1.5x speed
```

## Configuration

Create a `config.yaml` file to customize synthesis parameters:

```yaml
audio:
  # Crossfade between syllables (ms)
  crossfade_ms: 90

  # Crossfade for vowel endings (ms) - longer for smooth blending
  crossfade_vowel_ms: 140

  # Crossfade for S-endings (ms) - shorter for crisp sound
  crossfade_s_ending_ms: 30

  # Crossfade for R-endings (ms) - shorter for crisp sound
  crossfade_r_ending_ms: 30

  # Vowel-to-consonant factor (0.0-1.0)
  vowel_to_consonant_factor: 0.9

  # Silence between words (ms)
  word_pause_ms: 100

  # Remove silence within words
  remove_word_silence: true
  silence_threshold: 0.04
  min_silence_ms: 35

processing:
  remove_dc_offset: true
  normalize_level: 0.0
  compression: 0.0

synthesis:
  default_speed: 1.0
  min_speed: 0.5
  max_speed: 2.0

debug:
  print_units: false
  print_timing: false
```

## Database Format

The compiled database (`.db` file) contains:

| Section | Description |
|---------|-------------|
| Header (64 bytes) | Magic number, version, offsets |
| Index Table | Fixed-size entries for O(1) lookup |
| Hash Table | FNV-1a hash-based lookup |
| String Pool | UTF-8 text representations |
| Audio Data | Raw PCM (16-bit, 22050 Hz) |

## Audio Processing

### Crossfade Algorithm

Units are joined using raised-cosine crossfade for smooth transitions:

```
prev_gain = 0.5 * (1 + cos(π * t))  // 1 → 0
next_gain = 0.5 * (1 - cos(π * t))  // 0 → 1
```

### Special Cases

- **S-endings**: Shorter crossfade (30ms) preserves crisp sibilant sounds
- **R-endings**: Shorter crossfade (30ms) preserves clear R articulation
- **Vowel-to-vowel**: Longer crossfade (140ms) for natural flow
- **Vowel-to-consonant**: Moderate crossfade for balanced transition

### Time Stretching (PSOLA)

Speed adjustment uses Pitch-Synchronous Overlap-Add:
- Frame size: 20ms (441 samples at 22050 Hz)
- Hanning window for smooth reconstruction
- Preserves pitch while changing duration

## API

```c
// Initialize engine
CTTS* ctts_init(const char* database_file);

// Synthesize text
int ctts_synthesize(CTTS* engine, const char* text,
                    int16_t** samples, size_t* sample_count,
                    float speed);

// Write WAV file
int ctts_write_wav(const char* filename, const int16_t* samples,
                   size_t sample_count, int sample_rate);

// Normalization rules (loaded automatically from normalization.csv)
int ctts_load_normalization(const char* csv_file);
char* ctts_apply_normalization(const char* text);
void ctts_free_normalization(void);

// Cleanup
void ctts_free(CTTS* engine);
void ctts_free_samples(int16_t* samples);
```

## License

Copyright (c) 2024-2026

## Author

Built with assistance from Claude (Anthropic)
