#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <regex>
#include <nlohmann/json.hpp>
#include <codecvt>
#include <locale>

namespace stts {

class BPETokenizer {
public:
    BPETokenizer() = default;

    bool load(const std::string& vocab_path, const std::string& merges_path) {
        try {
            // Load vocab.json
            std::ifstream vocab_file(vocab_path);
            if (!vocab_file.is_open()) {
                std::cerr << "[BPETokenizer] Failed to open vocab file: " << vocab_path << std::endl;
                return false;
            }
            nlohmann::json vocab_json;
            vocab_file >> vocab_json;

            for (auto& el : vocab_json.items()) {
                token_to_id_[el.key()] = el.value();
                id_to_token_[el.value()] = el.key();
            }

            // Load merges.txt
            std::ifstream merges_file(merges_path);
            if (!merges_file.is_open()) {
                std::cerr << "[BPETokenizer] Failed to open merges file: " << merges_path << std::endl;
                return false;
            }

            std::string line;
            int priority = 0;

            // Skip first line if it's a version/comment (common in GPT-2 merges.txt)
            // Typically: "#version: 0.2"
            std::getline(merges_file, line);
            if (line.find("#version") == std::string::npos) {
                // If it doesn't look like a comment, maybe process it.
                // For safety, let's process it if it contains a space.
                if (line.find(' ') != std::string::npos) {
                   parse_merge_line(line, priority++);
                }
            }

            while (std::getline(merges_file, line)) {
                parse_merge_line(line, priority++);
            }

            // Initialize byte encoder (bytes -> unicode chars)
            init_byte_encoder();

            ready_ = true;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[BPETokenizer] Load error: " << e.what() << std::endl;
            return false;
        }
    }

    std::vector<int64_t> encode(const std::string& text) {
        if (!ready_) return {};

        std::vector<int64_t> bpe_tokens;

        // 1. Basic cleaning and splitting (Regex based pre-tokenization)
        // This regex is close to GPT-2's: 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
        // Since C++ std::regex doesn't fully support \p{L} consistently across platforms/compilers without ICU,
        // we will use a simplified whitespace-based splitting + punctuation handling for this "minimal" loader.
        // Or better: iterate through the string and handle space/non-space.

        // Simplified approach: split by whitespace but keep the leading space for the next word
        // Not perfect but functional for many cases.
        // Better: Use a simple regex to capture words and punctuation.

        // For robustness without heavy regex deps, let's treat the whole string or split by standard space.
        // GPT-2 BPE usually handles the whole string by mapping bytes to specific unicode chars.

        // Standard BPE pipeline:
        // Text -> Bytes -> Unicode Chars (via byte_encoder) -> BPE Merges -> Tokens

        // Step 1: Byte encoding (map each byte of input string to a unicode char)
        std::wstring ws;
        for (unsigned char c : text) {
            ws += byte_encoder_[c];
        }

        // Step 2: Pre-tokenize (split into words/chunks)
        // We will just do a simple split by space, but preserve the space as a character in the token.
        // Actually, let's try to implement a basic pre-tokenizer that splits on spaces but keeps them attached to the word.
        // E.g. "Hello world" -> "Hello", " world"

        // Simplest approximation: just process the whole string if it's short, or split by space.
        // But BPE works on words.
        std::vector<std::wstring> words = pre_tokenize(ws);

        for (const auto& word : words) {
            std::vector<std::wstring> word_chars;
            for (wchar_t wc : word) {
                word_chars.push_back(std::wstring(1, wc));
            }

            // Apply BPE
            auto merged = bpe(word_chars);

            // Map to IDs
            for (const auto& token : merged) {
                // We need to convert wstring token back to UTF-8 for map lookup
                std::string u8_token = wstring_to_utf8(token);
                if (token_to_id_.count(u8_token)) {
                    bpe_tokens.push_back(token_to_id_[u8_token]);
                } else {
                    // Fallback to <unk> or bytes?
                    // Usually GPT-2 covers all bytes so everything is tokenizable.
                    // If not found, we might skip or use unk.
                    // std::cerr << "Token not found: " << u8_token << std::endl;
                    if (token_to_id_.count("<|endoftext|>")) // Fallback
                         bpe_tokens.push_back(token_to_id_["<|endoftext|>"]);
                }
            }
        }

        return bpe_tokens;
    }

    /**
     * Decode token IDs back to text string.
     * This reverses the BPE encoding process.
     */
    std::string decode(const std::vector<int64_t>& token_ids) {
        if (!ready_) return "";

        std::string result;
        for (int64_t id : token_ids) {
            auto it = id_to_token_.find(id);
            if (it != id_to_token_.end()) {
                result += it->second;
            }
        }

        // Reverse byte encoder: convert unicode chars back to bytes
        // Build reverse mapping if needed
        if (byte_decoder_.empty()) {
            for (const auto& [byte_val, wchar] : byte_encoder_) {
                byte_decoder_[wchar] = byte_val;
            }
        }

        // Convert the result through byte decoder
        std::wstring wresult = utf8_to_wstring(result);
        std::vector<unsigned char> bytes;
        bytes.reserve(wresult.size());

        for (wchar_t wc : wresult) {
            auto dec_it = byte_decoder_.find(wc);
            if (dec_it != byte_decoder_.end()) {
                bytes.push_back(dec_it->second);
            } else {
                // Fallback: direct cast for standard ASCII
                if (wc < 256) {
                    bytes.push_back(static_cast<unsigned char>(wc));
                }
            }
        }

        return std::string(bytes.begin(), bytes.end());
    }

    /**
     * Check if a token ID represents a special token (e.g., control tokens).
     * Special tokens in Whisper: 50256 onwards are control tokens.
     */
    bool is_special_token(int64_t id) const {
        // Whisper special tokens start at 50256
        return id >= 50256;
    }

    /**
     * Get the token string for an ID without byte decoding (for debugging).
     */
    std::string get_token_str(int64_t id) const {
        auto it = id_to_token_.find(id);
        return (it != id_to_token_.end()) ? it->second : "";
    }

    bool is_ready() const { return ready_; }

private:
    bool ready_ = false;
    std::unordered_map<std::string, int64_t> token_to_id_;
    std::unordered_map<int64_t, std::string> id_to_token_;

    // Pair of strings -> rank/priority
    struct PairHash {
        std::size_t operator()(const std::pair<std::wstring, std::wstring>& p) const {
            return std::hash<std::wstring>{}(p.first) ^ std::hash<std::wstring>{}(p.second);
        }
    };
    std::unordered_map<std::pair<std::wstring, std::wstring>, int, PairHash> bpe_ranks_;

    // Byte encoder (byte value 0-255 -> unicode char)
    // GPT-2 uses a reversible mapping to avoid control chars etc.
    std::unordered_map<unsigned char, wchar_t> byte_encoder_;
    // Reverse mapping for decoding (unicode char -> byte value)
    mutable std::unordered_map<wchar_t, unsigned char> byte_decoder_;

    void parse_merge_line(const std::string& line, int priority = 0) {
        if (line.empty()) return;
        std::stringstream ss(line);
        std::string s1, s2;
        ss >> s1 >> s2;
        if (!s1.empty() && !s2.empty()) {
            bpe_ranks_[{utf8_to_wstring(s1), utf8_to_wstring(s2)}] = priority;
        }
    }

    void init_byte_encoder() {
        // Reference: https://github.com/openai/gpt-2/blob/master/src/encoder.py#L9
        // Bytes to unicode chars mapping to ensure reversibility and avoid control chars.
        std::vector<int> bs;
        // ASCII printables
        for (int i = '!'; i <= '~'; ++i) bs.push_back(i);
        // Latin-1 Supplement printables
        for (int i = 161; i <= 172; ++i) bs.push_back(i);
        for (int i = 174; i <= 255; ++i) bs.push_back(i);

        std::vector<int> cs = bs;
        int n = 0;
        for (int b = 0; b < 256; ++b) {
            if (std::find(bs.begin(), bs.end(), b) == bs.end()) {
                bs.push_back(b);
                cs.push_back(256 + n); // Map to higher unicode
                n++;
            }
        }

        for (size_t i = 0; i < bs.size(); ++i) {
            byte_encoder_[static_cast<unsigned char>(bs[i])] = static_cast<wchar_t>(cs[i]);
        }
    }

    std::vector<std::wstring> pre_tokenize(const std::wstring& text) {
        // Minimal split by space, keeping space at start of next word
        std::vector<std::wstring> words;
        std::wstring current_word;

        for (size_t i = 0; i < text.size(); ++i) {
            wchar_t c = text[i];
            if (c == L' ') {
                if (!current_word.empty()) {
                    words.push_back(current_word);
                }
                current_word = L" "; // Start new word with space
            } else {
                current_word += c;
            }
        }
        if (!current_word.empty()) words.push_back(current_word);

        return words;
    }

    std::vector<std::wstring> bpe(const std::vector<std::wstring>& input_word) {
        std::vector<std::wstring> word = input_word;

        while (word.size() > 1) {
            int min_rank = -1;
            std::pair<std::wstring, std::wstring> best_pair;
            int best_idx = -1;

            for (size_t i = 0; i < word.size() - 1; ++i) {
                std::pair<std::wstring, std::wstring> pair = {word[i], word[i+1]};
                if (bpe_ranks_.count(pair)) {
                    int rank = bpe_ranks_[pair];
                    if (min_rank == -1 || rank < min_rank) {
                        min_rank = rank;
                        best_pair = pair;
                        best_idx = i;
                    }
                }
            }

            if (min_rank == -1) break; // No more merges possible

            // Merge
            std::vector<std::wstring> new_word;
            size_t i = 0;
            while (i < word.size()) {
                if (i < word.size() - 1 && word[i] == best_pair.first && word[i+1] == best_pair.second) {
                    new_word.push_back(word[i] + word[i+1]);
                    i += 2;
                } else {
                    new_word.push_back(word[i]);
                    i++;
                }
            }
            word = new_word;
        }
        return word;
    }

    // Helpers for string conversion
    std::wstring utf8_to_wstring(const std::string& str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.from_bytes(str);
    }

    std::string wstring_to_utf8(const std::wstring& str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.to_bytes(str);
    }
};

} // namespace stts
