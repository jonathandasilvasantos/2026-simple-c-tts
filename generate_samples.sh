#!/bin/bash
#
# Generate Brazilian Portuguese TTS samples
# Tests all TTS features including prosody, punctuation, numbers, and normalizations
#

set -e

# Configuration
DATABASE="voice.db"
OUTPUT_DIR="./samples"
TTS="./ctts"

# Check if TTS executable exists
if [ ! -x "$TTS" ]; then
    echo "Error: TTS executable not found. Run 'make' first."
    exit 1
fi

# Check if database exists
if [ ! -f "$DATABASE" ]; then
    echo "Error: Database not found. Run './ctts build ./dataset voice.db' first."
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "Generating Brazilian Portuguese TTS samples..."
echo "Output directory: $OUTPUT_DIR"
echo ""

# Counter for tracking
count=0

# Function to generate a sample
generate() {
    local filename="$1"
    local text="$2"
    local speed="${3:-1.0}"

    count=$((count + 1))
    printf "[%02d] %s\n" "$count" "$text"
    $TTS synth "$DATABASE" "$text" "$OUTPUT_DIR/$filename" "$speed" > /dev/null 2>&1
}

# ============================================================================
# SECTION 1: Questions (Interrogation - Rising Intonation)
# ============================================================================
echo "=== Questions (Rising Intonation) ==="
generate "01_question_simple.wav" "como vai?"
generate "02_question_name.wav" "como você se chama?"
generate "03_question_where.wav" "onde você mora?"
generate "04_question_what.wav" "o que é isso?"
generate "05_question_when.wav" "quando você chega?"
generate "06_question_why.wav" "por que você fez isso?"
generate "07_question_how_much.wav" "quanto custa?"
generate "08_question_long.wav" "você pode me ajudar a encontrar o caminho?"
generate "09_question_yes_no.wav" "você fala português?"
generate "10_question_choice.wav" "você prefere café ou chá?"

# ============================================================================
# SECTION 2: Exclamations (Higher Energy/Pitch)
# ============================================================================
echo ""
echo "=== Exclamations (Higher Energy) ==="
generate "11_exclaim_wow.wav" "que legal!"
generate "12_exclaim_great.wav" "muito bom!"
generate "13_exclaim_amazing.wav" "isso é incrível!"
generate "14_exclaim_help.wav" "me ajuda!"
generate "15_exclaim_stop.wav" "para com isso!"
generate "16_exclaim_beautiful.wav" "que lindo!"
generate "17_exclaim_delicious.wav" "que delícia!"
generate "18_exclaim_congrats.wav" "parabéns!"
generate "19_exclaim_welcome.wav" "bem vindo!"
generate "20_exclaim_long.wav" "eu não acredito que isso aconteceu!"

# ============================================================================
# SECTION 3: Comma Pauses (Brief Pauses)
# ============================================================================
echo ""
echo "=== Comma Pauses ==="
generate "21_comma_list.wav" "eu quero café, pão, e manteiga"
generate "22_comma_address.wav" "olá, como vai você"
generate "23_comma_but.wav" "eu queria ir, mas não posso"
generate "24_comma_therefore.wav" "ele estudou muito, portanto passou"
generate "25_comma_series.wav" "vermelho, azul, verde, e amarelo"
generate "26_comma_clause.wav" "quando chegar em casa, me liga"
generate "27_comma_name.wav" "Maria, você pode vir aqui"
generate "28_comma_yes.wav" "sim, eu entendo"
generate "29_comma_no.wav" "não, obrigado"
generate "30_comma_complex.wav" "depois do almoço, vamos ao parque, e depois voltamos"

# ============================================================================
# SECTION 4: Period Pauses (Full Stops)
# ============================================================================
echo ""
echo "=== Period Pauses (Sentences) ==="
generate "31_period_two.wav" "eu gosto de música. ela também gosta."
generate "32_period_three.wav" "bom dia. como vai. tudo bem."
generate "33_period_story.wav" "era uma vez. havia um rei. ele era muito bom."
generate "34_period_instructions.wav" "primeiro abra a porta. depois entre. feche a porta."
generate "35_period_facts.wav" "o brasil é grande. tem muitas cidades. são paulo é a maior."

# ============================================================================
# SECTION 5: Mixed Punctuation
# ============================================================================
echo ""
echo "=== Mixed Punctuation ==="
generate "36_mixed_question_exclaim.wav" "você viu isso? que incrível!"
generate "37_mixed_comma_period.wav" "olá, tudo bem. sim, estou ótimo."
generate "38_mixed_all.wav" "espera, o que? não acredito! é verdade."
generate "39_mixed_dialogue.wav" "oi, como vai? bem, e você? também bem, obrigado!"
generate "40_mixed_complex.wav" "primeiro, pense bem. depois, decida. está pronto? então vamos!"

# ============================================================================
# SECTION 6: Number Expansion
# ============================================================================
echo ""
echo "=== Number Expansion ==="
generate "41_num_single.wav" "eu tenho 5 livros"
generate "42_num_teens.wav" "ela tem 15 anos"
generate "43_num_tens.wav" "são 42 pessoas"
generate "44_num_hundred.wav" "custa 100 reais"
generate "45_num_hundreds.wav" "são 350 quilômetros"
generate "46_num_thousand.wav" "tem 1000 lugares"
generate "47_num_thousands.wav" "são 2500 pessoas"
generate "48_num_year.wav" "estamos em 2024"
generate "49_num_big.wav" "a cidade tem 12000000 habitantes"
generate "50_num_mixed.wav" "eu tenho 3 filhos, 2 cachorros e 1 gato"

# ============================================================================
# SECTION 7: Abbreviations
# ============================================================================
echo ""
echo "=== Abbreviations ==="
generate "51_abbrev_dr.wav" "Dr. Silva é médico"
generate "52_abbrev_sra.wav" "Sra. Maria chegou"
generate "53_abbrev_prof.wav" "Prof. João ensina matemática"
generate "54_abbrev_units.wav" "são 5 km de distância"
generate "55_abbrev_weight.wav" "pesa 10 kg"
generate "56_abbrev_volume.wav" "tem 500 ml de água"
generate "57_abbrev_month.wav" "nasceu em jan. de 1990"
generate "58_abbrev_etc.wav" "comprei frutas, legumes, etc."
generate "59_abbrev_tel.wav" "meu tel. é novo"
generate "60_abbrev_mixed.wav" "Dr. Carlos mora a 3 km daqui"

# ============================================================================
# SECTION 8: Hiatos (Vowel Separation)
# ============================================================================
echo ""
echo "=== Hiatos (Vowel Separation) ==="
generate "61_hiato_praia.wav" "vamos para a praia"
generate "62_hiato_maio.wav" "nasceu em maio"
generate "63_hiato_feio.wav" "isso é muito feio"
generate "64_hiato_joia.wav" "que joia linda"
generate "65_hiato_apoio.wav" "preciso do seu apoio"
generate "66_hiato_saia.wav" "ela usa saia"
generate "67_hiato_areia.wav" "a areia é quente"
generate "68_hiato_ideia.wav" "que boa ideia"
generate "69_hiato_multiple.wav" "na praia, a areia é muito boa"
generate "70_hiato_sentence.wav" "em maio vou para a praia com a família"

# ============================================================================
# SECTION 9: R at Word Start (Double R Sound)
# ============================================================================
echo ""
echo "=== R at Word Start (RR Sound) ==="
generate "71_r_rosa.wav" "a rosa é vermelha"
generate "72_r_rio.wav" "o rio é grande"
generate "73_r_rato.wav" "o rato fugiu"
generate "74_r_rua.wav" "a rua está vazia"
generate "75_r_rei.wav" "o rei era bom"
generate "76_r_rico.wav" "ele é muito rico"
generate "77_r_roupa.wav" "comprei roupa nova"
generate "78_r_rapido.wav" "ele corre rápido"
generate "79_r_multiple.wav" "o rio rosa é raro"
generate "80_r_sentence.wav" "o rato roeu a roupa do rei de roma"

# ============================================================================
# SECTION 10: S Between Vowels (Z Sound)
# ============================================================================
echo ""
echo "=== S Between Vowels (Z Sound) ==="
generate "81_s_casa.wav" "minha casa é grande"
generate "82_s_mesa.wav" "a mesa está posta"
generate "83_s_rosa.wav" "a rosa cheira bem"
generate "84_s_coisa.wav" "que coisa estranha"
generate "85_s_preciso.wav" "eu preciso de ajuda"
generate "86_s_música.wav" "eu amo música"
generate "87_s_empresa.wav" "a empresa cresceu"
generate "88_s_brasil.wav" "o brasil é lindo"
generate "89_s_multiple.wav" "a casa rosa é preciosa"
generate "90_s_sentence.wav" "preciso comprar coisas para casa"

# ============================================================================
# SECTION 11: Word-Final T (TI Sound)
# ============================================================================
echo ""
echo "=== Word-Final T (TI Sound) ==="
generate "91_t_internet.wav" "a internet é rápida"
generate "92_t_eset.wav" "o set está pronto"

# ============================================================================
# SECTION 12: Declination (Pitch Drop Through Sentence)
# ============================================================================
echo ""
echo "=== Declination (Long Sentences) ==="
generate "93_decl_short.wav" "eu vou ao mercado comprar frutas"
generate "94_decl_medium.wav" "hoje de manhã eu acordei cedo e fui trabalhar"
generate "95_decl_long.wav" "quando eu era criança minha família morava em uma casa pequena perto do rio"
generate "96_decl_very_long.wav" "o brasil é um país muito grande com muitas cidades bonitas e pessoas simpáticas que adoram futebol e música"

# ============================================================================
# SECTION 13: Speed Variations (WSOLA Time-Stretching)
# ============================================================================
echo ""
echo "=== Speed Variations (WSOLA Time-Stretching) ==="

# Same phrase at different speeds to demonstrate pitch preservation
SPEED_TEST_PHRASE="o brasil é um país muito bonito"

generate "97_speed_0.5x.wav" "$SPEED_TEST_PHRASE" 0.5
generate "98_speed_0.7x.wav" "$SPEED_TEST_PHRASE" 0.7
generate "99_speed_0.8x.wav" "$SPEED_TEST_PHRASE" 0.8
generate "100_speed_1.0x.wav" "$SPEED_TEST_PHRASE" 1.0
generate "101_speed_1.2x.wav" "$SPEED_TEST_PHRASE" 1.2
generate "102_speed_1.5x.wav" "$SPEED_TEST_PHRASE" 1.5
generate "103_speed_1.8x.wav" "$SPEED_TEST_PHRASE" 1.8
generate "104_speed_2.0x.wav" "$SPEED_TEST_PHRASE" 2.0

# Different phrases at extreme speeds
echo ""
echo "=== Extreme Speed Tests ==="
generate "105_very_slow.wav" "esta frase está sendo falada bem devagar para testar" 0.5
generate "106_very_fast.wav" "esta frase está sendo falada muito rápido para testar" 2.0

# Speed comparison with questions and exclamations
echo ""
echo "=== Speed with Prosody ==="
generate "107_question_slow.wav" "você entendeu o que eu disse?" 0.7
generate "108_question_fast.wav" "você entendeu o que eu disse?" 1.5
generate "109_exclaim_slow.wav" "isso é incrível!" 0.7
generate "110_exclaim_fast.wav" "isso é incrível!" 1.5

# Long sentence at different speeds
LONG_PHRASE="quando eu era criança, minha família morava em uma casa pequena perto do rio"
echo ""
echo "=== Long Sentences at Different Speeds ==="
generate "111_long_slow.wav" "$LONG_PHRASE" 0.6
generate "112_long_normal.wav" "$LONG_PHRASE" 1.0
generate "113_long_fast.wav" "$LONG_PHRASE" 1.5

# Numbers at different speeds
echo ""
echo "=== Numbers at Different Speeds ==="
generate "114_numbers_slow.wav" "são 2500 reais e 50 centavos" 0.7
generate "115_numbers_fast.wav" "são 2500 reais e 50 centavos" 1.5

# ============================================================================
# SECTION 14: Complete Dialogues
# ============================================================================
echo ""
echo "=== Complete Dialogues ==="
generate "116_dialogue_greeting.wav" "olá, tudo bem? tudo ótimo, e você? também estou bem, obrigado!"
generate "117_dialogue_shopping.wav" "quanto custa isso? são 50 reais. está caro! posso fazer por 40."
generate "118_dialogue_directions.wav" "onde fica o banco? vira à direita, depois segue em frente. obrigado!"

# Dialogues at different speeds
echo ""
echo "=== Dialogues at Different Speeds ==="
generate "119_dialogue_slow.wav" "oi, como vai? bem, e você? também bem!" 0.7
generate "120_dialogue_fast.wav" "oi, como vai? bem, e você? também bem!" 1.5

echo ""
echo "============================================"
echo "Generated $count samples in $OUTPUT_DIR"
echo "============================================"

# List all generated files
echo ""
echo "Files:"
ls "$OUTPUT_DIR"/*.wav 2>/dev/null | while read file; do
    basename "$file"
done

echo ""
echo "Total size:"
du -sh "$OUTPUT_DIR"
