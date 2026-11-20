#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <memory>
#include <iostream>

namespace stts {

class SimpleG2P {
public:
    SimpleG2P() {
        init_phoneme_map();
    }

    ~SimpleG2P() {
        if (espeak_handle_) {
            auto espeak_Terminate = (int(*)())dlsym(espeak_handle_, "espeak_Terminate");
            if (espeak_Terminate) espeak_Terminate();
            dlclose(espeak_handle_);
        }
    }

    std::vector<int64_t> tokenize(const std::string& text) {
        if (!init_espeak()) {
            return tokenize_fallback(text);
        }

        std::string phonemes = phonemize(text);
        if (phonemes.empty()) {
            return tokenize_fallback(text);
        }

        return phonemes_to_tokens(phonemes);
    }

private:
    void* espeak_handle_ = nullptr;
    bool espeak_initialized_ = false;
    std::mutex espeak_mutex_;
    std::unordered_map<std::string, int64_t> phoneme_to_id_;

    bool init_espeak() {
        if (espeak_initialized_) return true;

        std::lock_guard<std::mutex> lock(espeak_mutex_);
        if (espeak_initialized_) return true;

        espeak_handle_ = dlopen("libespeak-ng.so.1", RTLD_LAZY);
        if (!espeak_handle_) {
            std::cerr << "[SimpleG2P] Failed to load libespeak-ng: " << dlerror() << std::endl;
            espeak_initialized_ = false;
            return false;
        }

        auto espeak_Initialize = (int(*)(int, int, const char*, int))dlsym(espeak_handle_, "espeak_Initialize");
        auto espeak_SetVoiceByName = (int(*)(const char*))dlsym(espeak_handle_, "espeak_SetVoiceByName");

        if (!espeak_Initialize || !espeak_SetVoiceByName) {
            std::cerr << "[SimpleG2P] Failed to load espeak symbols" << std::endl;
            dlclose(espeak_handle_);
            espeak_handle_ = nullptr;
            espeak_initialized_ = false;
            return false;
        }

        int sr = espeak_Initialize(0, 0, "/usr/lib/x86_64-linux-gnu/espeak-ng-data", 0);
        if (sr <= 0) {
            std::cerr << "[SimpleG2P] espeak_Initialize failed" << std::endl;
            dlclose(espeak_handle_);
            espeak_handle_ = nullptr;
            espeak_initialized_ = false;
            return false;
        }

        espeak_SetVoiceByName("en-us");
        espeak_initialized_ = true;
        return true;
    }

    std::string phonemize(const std::string& text) {
        if (!espeak_handle_ || !espeak_initialized_) return "";

        auto espeak_TextToPhonemes = (const char*(*)(const void**, int, int, int))
            dlsym(espeak_handle_, "espeak_TextToPhonemes");
        if (!espeak_TextToPhonemes) return "";

        std::string cleaned;
        cleaned.reserve(text.size());
        for (char c : text) {
            if (std::isalpha(c) || c == ' ' || c == '\'') {
                cleaned += std::tolower((unsigned char)c);
            } else if (c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':') {
                cleaned += ' ';
            }
        }

        const void* ptr = cleaned.c_str();
        const char* result = espeak_TextToPhonemes(&ptr, 1, 8, 2);
        return result ? std::string(result) : "";
    }

    std::vector<int64_t> phonemes_to_tokens(const std::string& phonemes) {
        std::vector<int64_t> tokens;

        // Espeak mnemonics sorted longest-first for greedy matching
        static const std::vector<std::string> mnemonic_list = {
            "aU", "aI", "eI", "oU", "OI", "3:", "a:", "e:", "i:", "o:", "u:",
            "dZ", "tS", "Ng", "N", "S", "Z", "D", "T",
            "@", "3", "E", "I", "{", "V", "O", "U", "A",
            "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
            "n", "o", "p", "r", "s", "t", "u", "v", "w", "x", "y", "z"
        };

        size_t i = 0;
        while (i < phonemes.size()) {
            char c = phonemes[i];

            // Skip stress markers, spaces, word boundaries
            if (c == '\'' || c == ',' || c == '#' || c == '-' || c == '_' || c == ' ') {
                i++;
                continue;
            }

            bool found = false;
            for (const auto& mnemonic : mnemonic_list) {
                if (phonemes.compare(i, mnemonic.size(), mnemonic) == 0) {
                    auto it = phoneme_to_id_.find(mnemonic);
                    if (it != phoneme_to_id_.end()) {
                        tokens.push_back(it->second);
                    } else {
                        tokens.push_back(1);
                    }
                    i += mnemonic.size();
                    found = true;
                    break;
                }
            }

            if (!found) {
                tokens.push_back(1);
                i++;
            }
        }

        return tokens;
    }

    std::vector<int64_t> tokenize_fallback(const std::string& text) {
        std::vector<int64_t> tokens;
        std::string processed = text;
        std::transform(processed.begin(), processed.end(), processed.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        std::stringstream ss(processed);
        std::string word;
        while (ss >> word) {
            for (char c : word) {
                std::string s(1, c);
                auto it = phoneme_to_id_.find(s);
                if (it != phoneme_to_id_.end()) {
                    tokens.push_back(it->second);
                } else {
                    tokens.push_back(1);
                }
            }
        }
        return tokens;
    }

    void init_phoneme_map() {
        int id = 0;
        phoneme_to_id_["$"] = id++;
        phoneme_to_id_[";"] = id++;
        phoneme_to_id_[":"] = id++;
        phoneme_to_id_[","] = id++;
        phoneme_to_id_["."] = id++;
        phoneme_to_id_["!"] = id++;
        phoneme_to_id_["?"] = id++;
        phoneme_to_id_[" "] = id++;

        id = 10;

        std::vector<std::string> symbols = {
            "b", "d", "f", "g", "h", "j", "k", "l", "m", "n", "p", "r", "s",
            "t", "v", "w", "z", "Z", "S", "D", "T", "N",
            "a", "e", "i", "o", "u", "@", "3", "E", "I", "{", "V", "O", "U", "A",
            "aI", "eI", "aU", "oU", "OI",
            "a:", "e:", "i:", "o:", "u:", "3:",
            "dZ", "tS", "Ng",
            "c", "q", "x", "y"
        };

        for (const auto& s : symbols) {
            if (phoneme_to_id_.find(s) == phoneme_to_id_.end()) {
                phoneme_to_id_[s] = id++;
            }
        }
    }
};

} // namespace stts
