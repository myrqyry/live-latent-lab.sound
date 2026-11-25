#pragma once
// Qualified Magenta DDSP-VST model pack, SHA256 of each original file.
namespace ddsp {
struct Asset {const char* name;size_t bytes;const char* sha;};
inline constexpr Asset instruments[]={
    {"Bassoon",7533210,"852971594144163cb24042d1f83c8398d6c622ab46685e51e778bc6532a98b6b"},
    {"Clarinet",7533204,"b7296bd686fe334fb43f17c6fbf34a1279ec94f1e22e61a790c0f4ea88ad917a"},
    {"Flute",7533210,"2d97ea37521d482d8d0572e22e040544df39be02a3729280df07e5ba364c8381"},
    {"Melodica",7532455,"4ec4bf8b5b526ed58e4f0247b8ec6f1964df680dffc8f15e3da292a5eaaed7f6"},
    {"Saxophone",7533206,"cfa54fb5041b2c0bc094636da7d46637c7b3b4b42d5e3ef89b6679958fd2e8e1"},
    {"Sitar",7532455,"012eb5fde07d2cc87ed160e23d1512c563948c0d84a94e1665b106d6d16f4e28"},
    {"Trombone",7533211,"8a15e13e6d7a46e6e804586598872a451b6490e06f0014822d06c99d310ef58c"},
    {"Trumpet",7533209,"82456d23ba1359c1c1bee5dd42a84907c02e4727364953c01be0fef2aaad3b79"},
    {"Tuba",7533211,"29f1163b7476f429710674023f2e3335faf15129efbe7dec1d7ae50714379ab9"},
    {"Violin",7533210,"2855fb285df5f66e37e6c12eef9409318c119a7e16ab90f1d1ada37f01a0673c"},
    {"Vowels",7533211,"29f4ffdf2d445faa496a8ac6c98e978af4dbb15962ddbb61fd2f0f12fbcd942d"},
};
inline constexpr Asset feature_asset={"extract_features_micro",671569,"98ade06772f9ca6ac64789fd843f14aeddf709084bbc76f060761b141837d0a2"};
inline constexpr Asset runtime_asset={"libLiteRt.so",8658680,"adb69308d4b2ff5d3116b3b7dd738802de3144f460dae2afb17790f031e24f0e"};
}
