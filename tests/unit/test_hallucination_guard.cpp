#include <gtest/gtest.h>
#include "utils/HallucinationGuard.h"

TEST(HallucinationGuard, NormalTextIsNotHallucination) {
    EXPECT_FALSE(isHallucination("Hola, buenos días. ¿Cómo estás?"));
}

TEST(HallucinationGuard, EmptyStringIsNotHallucination) {
    EXPECT_FALSE(isHallucination(""));
}

TEST(HallucinationGuard, SingleWordIsNotHallucination) {
    EXPECT_FALSE(isHallucination("hola"));
}

TEST(HallucinationGuard, TextOver500CharsIsHallucination) {
    std::string long_text(501, 'a');
    EXPECT_TRUE(isHallucination(long_text));
}

TEST(HallucinationGuard, Exactly500CharsIsNotHallucination) {
    // Boundary: 500 chars of distinct content must not trip the length guard
    // (uses single chars separated by spaces to avoid bigram/consecutive guards)
    std::string text;
    // Build a text with just short unique words up to ~500 chars
    for (int i = 0; text.size() < 498; ++i) {
        text += std::to_string(i) + " ";
    }
    text.resize(500);
    EXPECT_FALSE(isHallucination(text));
}

TEST(HallucinationGuard, FourConsecutiveIdenticalWordsIsHallucination) {
    EXPECT_TRUE(isHallucination("hola hola hola hola"));
}

TEST(HallucinationGuard, ThreeConsecutiveIdenticalWordsIsNotHallucination) {
    EXPECT_FALSE(isHallucination("hola hola hola mundo"));
}

TEST(HallucinationGuard, FourRepeatedBigramsIsHallucination) {
    // "la verdad" appears 4 times as a bigram
    EXPECT_TRUE(isHallucination("la verdad la verdad la verdad la verdad"));
}

TEST(HallucinationGuard, ThreeRepeatedBigramsIsNotHallucination) {
    EXPECT_FALSE(isHallucination("la verdad la verdad la verdad"));
}

TEST(HallucinationGuard, NormalLongSentenceIsNotHallucination) {
    EXPECT_FALSE(isHallucination(
        "El motor de transcripción procesa el audio en tiempo real "
        "usando una ventana deslizante con commit semántico basado "
        "en los timestamps de los segmentos de Whisper."));
}
