#include "translation_exporter.h"

#include "compat/optimized_translation_extractor.h"
#include "compat/resource_loader_compat.h"
#include "exporters/export_report.h"
#include "utility/common.h"
#include "utility/gd_parallel_hashmap.h"
#include "utility/gdre_settings.h"

#include "core/error/error_list.h"
#include "core/object/worker_thread_pool.h"
#include "core/string/optimized_translation.h"
#include "core/string/translation.h"
#include "core/string/ustring.h"
#include "modules/regex/regex.h"

Error TranslationExporter::export_file(const String &out_path, const String &res_path) {
	// Implementation for exporting translation files
	String iinfo_path = res_path.get_basename().get_basename() + ".csv.import";
	auto iinfo = ImportInfo::load_from_file(iinfo_path);
	ERR_FAIL_COND_V_MSG(iinfo.is_null(), ERR_CANT_OPEN, "Cannot find import info for translation.");
	Ref<ExportReport> report = export_resource(out_path.get_base_dir(), iinfo);
	ERR_FAIL_COND_V_MSG(report->get_error(), report->get_error(), "Failed to export translation resource.");
	return OK;
}
#ifdef DEBUG_ENABLED
#define bl_debug(...) print_line(__VA_ARGS__)
#else
#define bl_debug(...) print_verbose(__VA_ARGS__)
#endif

#define TEST_TR_KEY(key)                          \
	test = default_translation->get_message(key); \
	if (test == s) {                              \
		return key;                               \
	}                                             \
	key = key.to_upper();                         \
	test = default_translation->get_message(key); \
	if (test == s) {                              \
		return key;                               \
	}                                             \
	key = key.to_lower();                         \
	test = default_translation->get_message(key); \
	if (test == s) {                              \
		return key;                               \
	}

static const HashSet<char32_t> ALL_PUNCTUATION = { '.', '!', '?', ',', ';', ':', '(', ')', '[', ']', '{', '}', '<', '>', '/', '\\', '|', '`', '~', '@', '#', '$', '%', '^', '&', '*', '-', '_', '+', '=', '\'', '"', '\n', '\t', ' ' };
static const HashSet<char32_t> REMOVABLE_PUNCTUATION = { '.', '!', '?', ',', ';', ':', '%' };
static const Vector<String> STANDARD_SUFFIXES = { "Name", "Text", "Title", "Description", "Label", "Button", "Speech", "Tooltip", "Legend", "Body", "Content" };

static const char *MISSING_KEY_PREFIX = "<!MissingKey:";

template <class K, class V>
static constexpr _ALWAYS_INLINE_ const K &get_key(const KeyValue<K, V> &kv) {
	return kv.key;
}

template <class K, class V>
static constexpr _ALWAYS_INLINE_ const K &get_key(const std::pair<K, V> &kv) {
	return kv.first;
}

template <class K, class V>
static constexpr _ALWAYS_INLINE_ const V &get_value(const KeyValue<K, V> &kv) {
	return kv.value;
}

template <class K, class V>
static constexpr _ALWAYS_INLINE_ V &get_value(KeyValue<K, V> &kv) {
	return kv.value;
}

template <class K, class V>
static constexpr _ALWAYS_INLINE_ const V &get_value(const std::pair<K, V> &kv) {
	return kv.second;
}

template <class K, class V>
static constexpr _ALWAYS_INLINE_ V &get_value(std::pair<K, V> &kv) {
	return kv.second;
}

template <typename T>
void update_maximum(std::atomic<T> &maximum_value, T const &value) noexcept {
	T prev_value = maximum_value;
	while (prev_value < value &&
			!maximum_value.compare_exchange_weak(prev_value, value)) {
	}
}

template <class K, class V>
static constexpr _ALWAYS_INLINE_ bool map_has(const HashMap<K, V> &map, const K &key) {
	return map.has(key);
}

template <class K, class V>
static constexpr _ALWAYS_INLINE_ bool map_has(const ParallelFlatHashMap<K, V> &map, const K &key) {
	return map.contains(key);
}

bool map_has_str(const ParallelFlatHashMap<String, String> &map, const String &key) {
	return map.contains(key);
}

static Error write_to_csv(const String &output_path, const String &header, const Vector<String> &keys, const Vector<Vector<String>> &translation_messages) {
	Error err = gdre::ensure_dir(output_path.get_base_dir());
	ERR_FAIL_COND_V(err, err);
	Ref<FileAccess> f = FileAccess::open(output_path, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V(err, err);
	ERR_FAIL_COND_V(f.is_null(), err);
	// Set UTF-8 BOM (required for opening with Excel in UTF-8 format, works with all Godot versions)
	f->store_8(0xef);
	f->store_8(0xbb);
	f->store_8(0xbf);
	f->store_string(header);
	for (int i = 0; i < keys.size(); i++) {
		Vector<String> line_values;
		line_values.push_back(keys[i]);
		for (auto messages : translation_messages) {
			if (i >= messages.size()) {
				line_values.push_back("");
			} else {
				line_values.push_back(messages[i]);
			}
		}
		f->store_csv_line(line_values, ",");
	}
	f->flush();
	f->close();

	return OK;
}

struct KeyWorker {
	static constexpr int MAX_FILT_RES_STRINGS = 8000;
	static constexpr uint64_t MAX_STAGE_TIME = 30 * 1000ULL;

	using KeyType = String;
	using ValueType = String;
	using KeyMessageMap = HashMap<KeyType, ValueType>;

	Vector<KeyType> get_keys(const KeyMessageMap &map) {
		Vector<KeyType> ret;
		for (const auto &E : map) {
			ret.push_back(get_key(E));
		}
		return ret;
	}

	Mutex mutex;
	KeyMessageMap key_to_message;
	Vector<String> resource_strings;
	Vector<String> filtered_resource_strings;
	Vector<CharString> filtered_resource_strings_t;

	const Ref<OptimizedTranslationExtractor> default_translation;
	const Vector<String>& default_messages;
	const HashSet<String> previous_keys_found;

	Vector<String> keys;
	bool use_multithread = true;
	std::atomic<bool> keys_have_whitespace = false;
	std::atomic<bool> keys_are_all_upper = true;
	std::atomic<bool> keys_are_all_lower = true;
	std::atomic<bool> keys_are_all_ascii = true;
	bool has_common_prefix = false;
	bool do_stage_4 = true;
	bool do_stage_5 = false; // disabled for now, it's too slow
	std::atomic<bool> cancel = false;
	HashSet<char32_t> punctuation;
	HashSet<CharString> punctuation_str;

	std::atomic<size_t> keys_that_are_all_upper = 0;
	std::atomic<size_t> keys_that_are_all_lower = 0;
	std::atomic<size_t> keys_that_are_all_ascii = 0;
	std::atomic<size_t> max_key_len = 0;
	String common_to_all_prefix;
	Vector<String> common_prefixes;
	Vector<String> common_suffixes;
	Vector<CharString> common_suffixes_t;
	Vector<CharString> common_prefixes_t;

	ParallelFlatHashSet<String> successful_suffixes;
	ParallelFlatHashSet<String> successful_prefixes;

	Ref<RegEx> word_regex;
	std::atomic<uint64_t> current_keys_found = 0;
	Vector<uint64_t> times;
	Vector<uint64_t> keys_found;
	ParallelFlatHashSet<String> current_stage_keys_found;
	Vector<ParallelFlatHashSet<String>> stages_keys_found;
	std::atomic<uint64_t> last_completed = 0;
	// 30 seconds in msec
	uint64_t start_time = OS::get_singleton()->get_ticks_usec();
	uint64_t start_of_multithread = start_time;
	String default_locale;
	String old_translation_csv_path;
	String path;
	String current_stage;
	//default_translation,  default_messages;
	KeyWorker(const Ref<OptimizedTranslation> &p_default_translation,
			const Vector<String>& default_messages,
			const HashSet<String> &p_previous_keys_found) :
			default_translation(OptimizedTranslationExtractor::create_from(p_default_translation)),
			default_messages(default_messages),
			previous_keys_found(p_previous_keys_found) {
	}

	String sanitize_key(const String &s) {
		String str = s;
		str = str.replace("\n", "").replace(".", "").replace("…", "").replace("!", "").replace("?", "").strip_escapes().strip_edges();
		return str;
	}

	// make this a template that can take in either a HashMap or a HashMap
	//  use the is_flat_or_parallel_flat_hash_map trait
	static String find_common_prefix(const KeyMessageMap &key_to_msg) {
		// among all the keys in the vector, find the common prefix
		if (key_to_msg.size() == 0) {
			return "";
		}
		String prefix;
		auto add_to_prefix_func = [&](int i) {
			char32_t candidate = 0;
			for (const auto &E : key_to_msg) {
				auto &s = get_key(E);
				if (!s.is_empty()) {
					if (s.length() - 1 < i) {
						return false;
					}
					candidate = s[i];
					break;
				}
			}
			if (candidate == 0) {
				return false;
			}
			for (const auto &E : key_to_msg) {
				auto &s = get_key(E);
				if (!s.is_empty()) {
					if (s.length() - 1 < i || s[i] != candidate) {
						return false;
					}
				}
			}
			prefix += candidate;
			return true;
		};

		for (int i = 0; i < 100; i++) {
			if (!add_to_prefix_func(i)) {
				break;
			}
		}
		return prefix;
	}

	template <bool reverse = false>
	struct StringLengthCompare {
		static _ALWAYS_INLINE_ bool compare(const String &p_lhs, const String &p_rhs) {
			return reverse ? p_lhs.length() > p_rhs.length() : p_lhs.length() < p_rhs.length();
		}

		_ALWAYS_INLINE_ bool operator()(const Variant &p_lhs, const Variant &p_rhs) const {
			return compare(p_lhs, p_rhs);
		}
	};

	template <typename T>
	void find_common_prefixes_and_suffixes(const Vector<T> &res_strings, int count_threshold = 3, bool clear = false) {
		HashMap<String, int> prefix_counts;
		HashMap<String, int> suffix_counts;

		if (clear) {
			common_prefixes.clear();
			common_suffixes.clear();
		}
		auto inc_counts = [&](HashMap<String, int> &counts, const String &part) {
			if (part.is_empty()) {
				return;
			}
			if (counts.has(part)) {
				counts[part] += 1;
			} else {
				counts[part] = 1;
			}
		};

		for (const auto &res_s : res_strings) {
			if (res_s.is_empty()) {
				continue;
			}
			auto parts = gdre::split_multichar(res_s, punctuation, false, 0);
			String prefix = parts.size() > 0 ? parts[0] : "";
			inc_counts(prefix_counts, prefix);
			for (int i = 1; i < parts.size() - 1; i++) {
				auto &part = parts[i];
				int part_start_idx = prefix.length();
				while (part_start_idx < res_s.length()) {
					auto chr = res_s[part_start_idx];
					if (punctuation.has(chr)) {
						prefix += chr;
					} else {
						break;
					}
					part_start_idx++;
				}
				prefix += part;
				inc_counts(prefix_counts, prefix);
			}
			auto suffix_parts = gdre::split_multichar(res_s, punctuation, false, 0);
			String suffix = suffix_parts.size() > 0 ? suffix_parts[suffix_parts.size() - 1] : "";
			inc_counts(suffix_counts, suffix);
			// check if the suffix ends with a number
			if (suffix.is_empty()) {
				continue;
			}
			int end_pad = 0;
			char32_t last_char = suffix[suffix.length() - 1];
			if (last_char >= '0' && last_char <= '9') {
				// strip the trailing numbers
				while (suffix.length() > 0) {
					last_char = suffix[suffix.length() - 1];
					if ((last_char >= '0' && last_char <= '9') || (punctuation.has(last_char))) {
						suffix = suffix.substr(0, suffix.length() - 1);
						end_pad++;
					} else {
						break;
					}
				}
				inc_counts(suffix_counts, suffix);
			}

			for (int i = suffix_parts.size() - 2; i > 0; i--) {
				auto &part = suffix_parts[i];
				int part_end_idx = res_s.length() - (suffix.length() + end_pad) - 1;
				while (part_end_idx > 0) {
					auto chr = res_s[part_end_idx];
					if (punctuation.has(chr)) {
						suffix = chr + suffix;
					} else {
						break;
					}
					part_end_idx--;
				}
				suffix = part + suffix;
				inc_counts(suffix_counts, suffix);
			}
		}
		for (const auto &E : prefix_counts) {
			if (get_value(E) >= count_threshold && !common_prefixes.has(get_key(E))) {
				common_prefixes.push_back(get_key(E));
			}
		}
		for (const auto &E : suffix_counts) {
			if (get_value(E) >= count_threshold && !common_suffixes.has(get_key(E))) {
				common_suffixes.push_back(get_key(E));
			}
		}
		// sort the prefixes and suffixes by length

		common_prefixes.sort_custom<StringLengthCompare<true>>();
		common_suffixes.sort_custom<StringLengthCompare<true>>();
	}

	_FORCE_INLINE_ void _set_key_stuff(const String &key) {
		++current_keys_found;
		if (!keys_have_whitespace && gdre::string_has_whitespace(key)) {
			keys_have_whitespace = true;
		}
		if (key.to_upper() == key) {
			keys_that_are_all_upper++;
		} else {
			keys_are_all_upper = false;
		}
		if (key.to_lower() == key) {
			keys_that_are_all_lower++;
		} else {
			keys_are_all_lower = false;
		}
		if (gdre::string_is_ascii(key)) {
			keys_that_are_all_ascii++;
		} else {
			keys_are_all_ascii = false;
		}
		current_stage_keys_found.insert(key);
		update_maximum(max_key_len, (size_t)key.length());
		gdre::get_chars_in_set(key, ALL_PUNCTUATION, punctuation);
		for (char32_t p : punctuation) {
			punctuation_str.insert(String::chr(p).utf8());
		}
	}

	_FORCE_INLINE_ bool _set_key(const String &key, const String &msg) {
		MutexLock lock(mutex);
		if (key.is_empty()) {
			return false;
		}
		if (map_has(key_to_message, key)) {
			return true;
		}
		_set_key_stuff(key);

		key_to_message[key] = msg;
		return true;
	}

	_FORCE_INLINE_ bool try_key(const String &key) {
		if (key.is_empty()) {
			return false;
		}
		auto msg = default_translation->get_message_str(key);
		if (!msg.is_empty()) {
			return _set_key(key, msg);
		}
		return false;
	}

	_FORCE_INLINE_ bool try_key(const char *key) {
		if (key[0] == '\0') {
			return false;
		}
		auto msg = default_translation->get_message_str(key);
		if (!msg.is_empty()) {
			return _set_key(key, msg);
		}
		return false;
	}

	constexpr bool is_empty_or_null(const char *str) {
		return !str || *str == 0;
	}

	String combine_string(const char *part1, const char *part2 = "", const char *part3 = "", const char *part4 = "", const char *part5 = "", const char *part6 = "") {
		auto str = String::utf8(part1);
		if (!is_empty_or_null(part2)) {
			str += String::utf8(part2);
		}
		if (!is_empty_or_null(part3)) {
			str += String::utf8(part3);
		}
		if (!is_empty_or_null(part4)) {
			str += String::utf8(part4);
		}
		if (!is_empty_or_null(part5)) {
			str += String::utf8(part5);
		}
		if (!is_empty_or_null(part6)) {
			str += String::utf8(part6);
		}
		return str;
	}

	void reg_successful_prefix(const String &prefix) {
#ifdef DEBUG_ENABLED
		if (!prefix.is_empty()) {
			successful_prefixes.insert(prefix);
		}
#endif
	}

	void reg_successful_suffix(const String &suffix) {
#ifdef DEBUG_ENABLED
		if (!suffix.is_empty()) {
			successful_suffixes.insert(suffix);
		}
#endif
	}

	_FORCE_INLINE_ bool try_key_multipart(const char *part1, const char *part2 = "", const char *part3 = "", const char *part4 = "", const char *part5 = "", const char *part6 = "") {
		auto msg = default_translation->get_message_multipart_str(part1, part2, part3, part4, part5, part6);
		if (!msg.is_empty()) {
			auto key = combine_string(part1, part2, part3, part4, part5, part6);
			_set_key(key, msg);
			return true;
		}
		return false;
	}

	bool try_key_prefix(const char *prefix, const char *suffix) {
		if (try_key_multipart(prefix, suffix)) {
			reg_successful_prefix(suffix);
			return true;
		}
		for (auto p : punctuation_str) {
			if (try_key_multipart(prefix, p.get_data(), suffix)) {
				reg_successful_prefix(suffix);
				return true;
			}
		}
		return false;
	}

	bool try_key_suffix(const char *prefix, const char *suffix) {
		if (try_key_multipart(prefix, suffix)) {
			reg_successful_suffix(suffix);
			return true;
		}
		for (auto p : punctuation_str) {
			if (try_key_multipart(prefix, p.get_data(), suffix)) {
				reg_successful_suffix(suffix);
				return true;
			}
		}
		return false;
	}

	bool try_key_suffixes(const char *prefix, const char *suffix, const char *suffix2) {
		bool suffix1_empty = !suffix || *suffix == 0;
		if (suffix1_empty) {
			return try_key_suffix(prefix, suffix2);
		}
		if (try_key_multipart(prefix, suffix, suffix2)) {
			reg_successful_suffix(combine_string(suffix, suffix2));
			return true;
		}
		for (auto p : punctuation_str) {
			if (try_key_multipart(prefix, suffix, p.get_data(), suffix2)) {
				reg_successful_suffix(combine_string(suffix, p.get_data(), suffix2));
				return true;
			}
		}
		return false;
	}

	bool try_key_prefix_suffix(const char *prefix, const char *key, const char *suffix) {
		if (try_key_multipart(prefix, key, suffix)) {
			reg_successful_prefix(combine_string(prefix));
			reg_successful_suffix(combine_string(suffix));
			return true;
		}
		for (auto p : punctuation_str) {
			if (try_key_multipart(prefix, p.get_data(), key, p.get_data(), suffix)) {
				reg_successful_prefix(combine_string(prefix));
				reg_successful_suffix(combine_string(suffix));
				return true;
			}
		}
		return false;
	}

	CharString cs_num(int64_t num, int zero_prefix_len) {
		CharString ret;
		ret.resize_uninitialized(32);
		const char *format;
		if (zero_prefix_len > 0) {
			if (zero_prefix_len == 1) {
				format = "%02lld";
			} else if (zero_prefix_len == 2) {
				format = "%03lld";
			} else if (zero_prefix_len == 3) {
				format = "%04lld";
			} else if (zero_prefix_len == 4) {
				format = "%05lld";
			} else if (zero_prefix_len == 5) {
				format = "%06lld";
			} else if (zero_prefix_len == 6) {
				format = "%07lld";
			} else {
				format = "%08lld";
			}
		} else {
			format = "%lld";
		}
		int len = snprintf(ret.ptrw(), 31, format, num);
		ret.resize_uninitialized(len + 1);
		return ret;
	}

	auto try_strip_numeric_suffix(const char *p_res_s, int &num_suffix_val) {
		size_t res_s_len = strlen(p_res_s);
		if (res_s_len < 2) {
			return CharString(p_res_s);
		}
		char last_char = p_res_s[res_s_len - 1];
		bool stripped_last_char = false;
		const char *res_s = p_res_s;
		int new_len = res_s_len;
		while (last_char >= '0' && last_char <= '9') {
			stripped_last_char = true;
			new_len = new_len - 1;
			if (new_len == 0) {
				stripped_last_char = false;
				break;
			}
			last_char = p_res_s[new_len - 1];
		}
		CharString res_s_copy;
		String num_str;
		num_suffix_val = -1;
		if (stripped_last_char) {
			// malloc a new string
			res_s_copy.resize_uninitialized(new_len + 1);
			memcpy(res_s_copy.ptrw(), p_res_s, new_len);
			res_s_copy[new_len] = '\0';
			num_suffix_val = String(p_res_s + new_len).to_int();
		} else {
			res_s_copy = p_res_s;
		}
		return res_s_copy;
	}

	auto try_strip_numeric_suffix(const CharString &p_res_s, int &magnitude) {
		size_t res_s_len = p_res_s.size();
		if (res_s_len < 2) {
			return p_res_s;
		}
		char last_char = p_res_s[res_s_len - 2];
		bool stripped_last_char = false;
		int new_len = res_s_len;
		while (last_char >= '0' && last_char <= '9') {
			stripped_last_char = true;
			new_len = new_len - 1;
			if (new_len == 0) {
				stripped_last_char = false;
				break;
			}
			last_char = p_res_s[new_len - 1];
		}
		CharString res_s_copy;
		String num_str;

		if (stripped_last_char) {
			res_s_copy = p_res_s;
			res_s_copy.resize_uninitialized(new_len + 1);
			res_s_copy[new_len] = '\0';
			String num_str = String(p_res_s.get_data() + new_len);
			// check how many zeros are in the num_str
			int zero_count = 0;
			for (int i = 0; i < num_str.length(); i++) {
				if (num_str[i] == '0') {
					zero_count++;
				} else {
					break;
				}
			}
			magnitude = zero_count;
		} else {
			magnitude = -1;
			return p_res_s;
		}
		return res_s_copy;
	}

	constexpr const char *get_magnitude_prefix(int magnitude) {
		switch (magnitude) {
			case 0:
				return "";
			case 1:
				return "0";
			case 2:
				return "00";
			case 3:
				return "000";
			case 4:
				return "0000";
			case 5:
				return "00000";
			case 6:
				return "000000";
			case 7:
				return "0000000";
			case 8:
				return "00000000";
			case 9:
				return "000000000";
			case 10:
				return "0000000000";
			default:
				return "";
		}
	}

	auto try_num_suffix(const char *res_s, const char *suffix = "", bool skip_magnitude_check = false) {
		bool found_num = try_key_suffixes(res_s, suffix, "1");
		int zero_prefix_len = 0;
		if (!skip_magnitude_check) {
			zero_prefix_len = try_key_suffixes(res_s, suffix, "01") ? 1 : 0;
			if (!found_num && zero_prefix_len == 0) {
				zero_prefix_len = try_key_suffixes(res_s, suffix, "001") ? 2 : 0;
				if (zero_prefix_len == 0) {
					zero_prefix_len = try_key_suffixes(res_s, suffix, "0001") ? 3 : 0;
				}
			}
		}
		if (found_num || zero_prefix_len > 0 || skip_magnitude_check) {
			try_key_suffixes(res_s, suffix, "N");
			try_key_suffixes(res_s, suffix, "n");
			try_key_suffixes(res_s, suffix, "0");
			bool found_most = true;
			int min_num = skip_magnitude_check ? 0 : 2;
			int max_num = skip_magnitude_check ? 10 : 4;

			while (found_most) {
				int numbers_found = 0;
				for (int num = min_num; num < max_num; num++) {
					if (try_key_suffixes(res_s, suffix, cs_num(num, zero_prefix_len).get_data())) {
						numbers_found++;
					}
				}
				if (numbers_found >= max_num - min_num - 1) {
					found_most = true;
				} else {
					found_most = false;
				}
				min_num = max_num;
				max_num = max_num * 2;
			}
		}
	}

	void prefix_suffix_task_2(uint32_t i, CharString *res_strings) {
		if (unlikely(cancel)) {
			return;
		}
		const CharString &res_s = res_strings[i];
		try_num_suffix(res_s.get_data());

		for (const auto &E : common_suffixes_t) {
			try_key_suffix(res_s.get_data(), E.get_data());
			try_num_suffix(res_s.get_data(), E.get_data());
		}
		for (const auto &E : common_prefixes_t) {
			try_key_prefix(E.get_data(), res_s.get_data());
			try_num_suffix(E.get_data(), res_s.get_data());
		}
		last_completed++;
	}

	void stage_3_5_task(uint32_t i, Pair<CharString, int> *res_strings) {
		if (unlikely(cancel)) {
			return;
		}
		const Pair<CharString, int> &res_s_pair = res_strings[i];
		const char *res_s_data = res_s_pair.first.get_data();
		int magnitude = res_s_pair.second;
		try_num_suffix(res_s_data, get_magnitude_prefix(magnitude), magnitude != -1);
		last_completed++;
	}

	void partial_task(uint32_t i, String *res_strings) {
		if (unlikely(cancel)) {
			return;
		}
		const String &res_s = res_strings[i];
		if (!has_common_prefix || res_s.contains(common_to_all_prefix)) {
			auto matches = word_regex->search_all(res_s);
			for (const Ref<RegExMatch> match : matches) {
				for (const String &key : match->get_strings()) {
					try_key(key);
				}
			}
		}
		last_completed++;
	}

	void stage_5_task_2(uint32_t i, CharString *res_strings) {
		if (unlikely(cancel)) {
			return;
		}
		const CharString &res_s = res_strings[i];
		auto frs_size = filtered_resource_strings.size();
		for (uint32_t j = 0; j < frs_size; j++) {
			const CharString &res_s2 = res_strings[j];
			try_key_suffix(res_s.get_data(), res_s2.get_data());
		}
		++last_completed;
	}

	void end_stage() {
		last_completed = 0;
		cancel = false;
		times.push_back(OS::get_singleton()->get_ticks_msec());
		keys_found.push_back(current_keys_found);
		stages_keys_found.push_back(current_stage_keys_found);
		current_keys_found = 0;
		current_stage_keys_found.clear();
	}

	static bool check_for_timeout(const uint64_t start_time, const uint64_t max_time) {
		if ((OS::get_singleton()->get_ticks_msec() - start_time) > max_time) {
			return true;
		}
		return false;
	}

	Error wait_for_task(WorkerThreadPool::GroupID group_task, const String &stage_name, size_t size, uint64_t max_time) {
		uint64_t next_report = 5000;
		uint64_t start_time = OS::get_singleton()->get_ticks_msec();
		while (!WorkerThreadPool::get_singleton()->is_group_task_completed(group_task)) {
			// wait 100ms
			OS::get_singleton()->delay_usec(100000);
			if (check_for_timeout(start_time, max_time)) {
				bl_debug("Timeout waiting for " + stage_name + " to complete...");
				cancel = true;
				WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group_task);
				return ERR_TIMEOUT;
			}
			if (check_for_timeout(start_time, next_report)) {
				bl_debug("waiting for " + stage_name + " to complete... (" + itos(last_completed) + "/" + itos(size) + ")");
				next_report += 5000;
			}
		}

		// Always wait for completion; otherwise we leak memory.
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group_task);
		bl_debug(stage_name + " completed!");
		return OK;
	}

	// Does not filter based on spaces
	bool has_nonspace_and_std_punctuation(const String &s) {
		for (int i = 0; i < s.length(); i++) {
			char32_t c = s.ptr()[i];
			if (c != ' ' && !punctuation.has(c) && ALL_PUNCTUATION.has(c)) {
				return true;
			}
		}
		return false;
	}

	bool should_filter(const String &res_s, bool ignore_spaces = false) {
		if (res_s.is_empty()) {
			return true;
		}
		if (res_s.size() > max_key_len) {
			return true;
		}

		// if (filter_punctuation) {
		if (has_nonspace_and_std_punctuation(res_s)) {
			return true;
		}
		// contains any whitespace
		if (!ignore_spaces && !keys_have_whitespace && gdre::string_has_whitespace(res_s)) {
			return true;
		}
		if (res_s.begins_with("res://")) {
			return true;
		}
		if (!common_to_all_prefix.is_empty() && !res_s.begins_with(common_to_all_prefix)) {
			return true;
		}
		if (keys_are_all_upper && res_s.to_upper() != res_s) {
			return true;
		}
		if (keys_are_all_lower && res_s.to_lower() != res_s) {
			return true;
		}
		if (keys_are_all_ascii && !gdre::string_is_ascii(res_s)) {
			return true;
		}
		return false;
	}

	String remove_removable_punct(const String &s) {
		String ret;
		for (int i = 0; i < s.length(); i++) {
			char32_t c = s.ptr()[i];
			if (!punctuation.has(c) && REMOVABLE_PUNCTUATION.has(c)) {
				ret += c;
			}
		}
		return ret;
	}

	template <class T>
	Vector<String> get_sanitized_strings(const Vector<T> &default_messages) {
		static_assert(std::is_same<T, String>::value || std::is_same<T, StringName>::value, "T must be either String or StringName");
		HashSet<String> new_strings;
		for (const T &msg : default_messages) {
			auto msg_str = remove_removable_punct(msg).strip_escapes().strip_edges();
			for (auto ch : punctuation) {
				// strip edges
				msg_str = msg_str.trim_suffix(String::chr(ch)).trim_prefix(String::chr(ch));
			}
			if (has_nonspace_and_std_punctuation(msg_str)) {
				continue;
			}
			if (keys_are_all_ascii && !gdre::string_is_ascii(msg_str)) {
				continue;
			}
			if (keys_are_all_upper) {
				msg_str = msg_str.to_upper();
			} else if (keys_are_all_lower) {
				msg_str = msg_str.to_lower();
			}
			if (msg_str.contains(" ")) {
				for (char32_t p : punctuation) {
					auto nar = msg_str.replace(" ", String::chr(p));
					new_strings.insert(nar);
				}
			} else {
				new_strings.insert(msg_str);
			}
		}
		return gdre::hashset_to_vector(new_strings);
	}

	void get_sanitized_message_strings(Vector<String> &new_strings) {
		auto hshset = gdre::vector_to_hashset(filtered_resource_strings);
		for (const auto &msg_str : get_sanitized_strings(default_messages)) {
			if (hshset.has(msg_str)) {
				continue;
			}
			hshset.insert(msg_str);
			new_strings.push_back(msg_str);
		}
	}

	void extract_middles(const Vector<String> &frs, Vector<String> &middles) {
		auto old_hshset = gdre::vector_to_hashset(frs);
		auto hshset = gdre::vector_to_hashset(frs);
		auto insert_into_hashset = [&](const String &s) {
			if (hshset.has(s)) {
				return false;
			}
			hshset.insert(s);
			middles.push_back(s);
			return true;
		};
		auto trim_punctuation = [&](const String &s) {
			auto ret = s;
			for (auto ch : punctuation) {
				ret = ret.trim_suffix(String::chr(ch)).trim_prefix(String::chr(ch));
			}
			return ret;
		};
		for (auto &res_s : frs) {
			// String s = res_s;
			for (auto &prefix : common_prefixes) {
				if (prefix.length() != res_s.length() && res_s.begins_with(prefix)) {
					auto s = trim_punctuation(res_s.substr(prefix.length()));
					if (!insert_into_hashset(s)) {
						continue;
					}
					for (auto &suffix : common_suffixes) {
						if (suffix.length() != s.length() && s.ends_with(suffix)) {
							auto t = trim_punctuation(s.substr(0, s.length() - suffix.length()));
							insert_into_hashset(t);
						}
					}
				}
			}
			for (auto &suffix : common_suffixes) {
				if (suffix.length() != res_s.length() && res_s.ends_with(suffix)) {
					auto s = trim_punctuation(res_s.substr(0, res_s.length() - suffix.length()));
					insert_into_hashset(s);
				}
			}
		}
	}

	// TODO: Rise of the Golden Idol specific hack, remove this
	void dynamic_rgi_hack() {
#if 0
		const String ITEM_TR_SEP = "|";
		const String ITEM_TR = "DB_%d";
		const String ITEM_TR_PREFIX_ARC = "ARC";
		int min_scenario_id = 0;
		int max_scenario_id = 100;
		int min_arc_id = 0;
		int max_arc_id = 10;
		int max_item_id = 10000;
		auto get_translation_id = [&](int id) {
			return vformat(ITEM_TR, id);
		};
		auto get_composite_translation_id = [&](int scenario_id, int item_id) {
			return vformat("%d%s%s", scenario_id, ITEM_TR_SEP, get_translation_id(item_id));
		};
		auto get_composite_arc_translation_id = [&](int arc_id, int item_id) {
			auto prefix = vformat("%s%d", ITEM_TR_PREFIX_ARC, arc_id);
			return vformat("%s%s%s", prefix, ITEM_TR_SEP, get_translation_id(item_id));
		};
		for (int item_id = 0; item_id < max_item_id; item_id++) {
			try_key(get_translation_id(item_id));
			for (int scenario_id = min_scenario_id; scenario_id < max_scenario_id; scenario_id++) {
				try_key(get_composite_translation_id(scenario_id, item_id));
			}
			for (int arc_id = min_arc_id; arc_id < max_arc_id; arc_id++) {
				try_key(get_composite_arc_translation_id(arc_id, item_id));
			}
		}
#endif
	}

	String get_step_desc(uint32_t i, void *userdata) {
		return "Searching for keys for " + path.get_file() + "... (" + current_stage + "/4) ";
	}

	template <typename M, class VE>
	Error run_stage(M p_multi_method, Vector<VE> p_userdata, const String &stage_name, bool multi = true) {
		// assert that M is a method belonging to this class
		last_completed = 0;
		auto desc = "TranslationExporter::find_missing_keys::" + stage_name;
		cancel = false;
		current_stage = stage_name;
		static_assert(std::is_member_function_pointer<M>::value, "M must be a method of this class");
		int tasks = 1;
		if (multi) {
			tasks = -1;
		}
		Error err = TaskManager::get_singleton()->run_multithreaded_group_task(
				this,
				p_multi_method,
				p_userdata.ptrw(),
				p_userdata.size(),
				&KeyWorker::get_step_desc,
				KeyWorker::get_step_desc(0, nullptr),
				stage_name, true, tasks, true);

		end_stage();
		return err;
	}

	bool met_threshold() {
		return (double)default_messages.size() / (double)key_to_message.size() > ((double)1 - TranslationExporter::threshold);
	}

	void pop_charstr_vectors() {
		filtered_resource_strings_t.clear();
		common_prefixes_t.clear();
		common_suffixes_t.clear();
		for (const auto &E : filtered_resource_strings) {
			filtered_resource_strings_t.push_back(E.utf8());
		}
		for (const auto &E : common_prefixes) {
			common_prefixes_t.push_back(E.utf8());
		}
		for (const auto &E : common_suffixes) {
			common_suffixes_t.push_back(E.utf8());
		}
	}

	void stage_1(uint32_t i, String *resource_strings) {
		const String &key = resource_strings[i];
		try_key(key);
	}

	int64_t pop_keys() {
		int64_t missing_keys = 0;
		keys.clear();

		for (int i = 0; i < default_messages.size(); i++) {
			auto &msg = default_messages[i];
			bool found = false;
			bool has_match = false;
			String matching_key;
			for (const auto &E : key_to_message) {
				DEV_ASSERT(!get_value(E).is_empty());

				if (get_value(E) == msg) {
					has_match = true;
					matching_key = get_key(E);
					if (!keys.has(get_key(E))) {
						keys.push_back(get_key(E));
						found = true;
						break;
					}
				}
			}
			if (!found) {
				if (has_match) {
					if (const auto &matching_message = key_to_message[matching_key]; msg != matching_message) {
						WARN_PRINT(vformat("Found matching key '%s' for message '%s' but key is used for message '%s'", matching_key, msg, matching_message));
					} else {
						print_verbose(vformat("WARNING: Found duplicate key '%s' for message '%s'", matching_key, msg));
						keys.push_back(matching_key);
						continue;
					}
				} else {
					print_verbose(vformat("Could not find key for message '%s'", msg));
				}
				missing_keys++;
				keys.push_back(MISSING_KEY_PREFIX + String(msg).split("\n")[0] + ">");
			}
		}
		return missing_keys;
	}

	int64_t run() {
		cancel = false;
		uint64_t missing_keys = 0;
		HashSet<String> res_strings;
		start_time = OS::get_singleton()->get_ticks_msec();
		auto progress = EditorProgressGDDC::create(nullptr, "TranslationExporter - " + path, "Exporting translation " + path + "...", -1, true);

		// hint file read
		const String translation_hint_file_path = GDRESettings::get_singleton()->get_translation_hint_file_path();
		if (!translation_hint_file_path.is_empty()) {
			Ref<FileAccess> f = FileAccess::open(translation_hint_file_path, FileAccess::READ);
			while (f.is_valid() && !f->eof_reached()) {
				String line = f->get_line();
				if (!line.is_empty()) {
					try_key(line);
				}
			}
		}

		// old translation csv read
		if (!old_translation_csv_path.is_empty()) {
			Ref<FileAccess> f = FileAccess::open(old_translation_csv_path, FileAccess::READ);
			while (f.is_valid() && !f->eof_reached()) {
				Vector<String> line = f->get_csv_line();
				if (line.size() <= 1) {
					continue;
				}
				if (line[0].is_empty()) {
					continue;
				}
				try_key(line[0]);
			}
		}

		// Stage 1: Unmodified resource strings
		// We need to load all the resource strings in all resources to find the keys
		if (!GDRESettings::get_singleton()->loaded_resource_strings()) {
			GDRESettings::get_singleton()->load_all_resource_strings();
		}
		GDRESettings::get_singleton()->get_resource_strings(res_strings);
		resource_strings = gdre::hashset_to_vector(res_strings);
		Error err = run_stage(&KeyWorker::stage_1, resource_strings, "Stage 1", false);
		if (err != OK) {
			return pop_keys();
		}

		// Stage 1.25: try the messages themselves
		for (const String &message : default_messages) {
			try_key(message);
		}

		// Stage 1.5: Previous keys found
		if (key_to_message.size() != default_messages.size()) {
			for (const String &key : previous_keys_found) {
				try_key(key);
			}
		}
		// Stage 1.75: dynamic_rgi_hack
		dynamic_rgi_hack();
		end_stage();
		common_to_all_prefix = find_common_prefix(key_to_message);
		has_common_prefix = !common_to_all_prefix.is_empty();

		// Stage 2: Partial resource strings
		// look for keys in every PART of the resource strings
		// Only do this if no keys have spaces or punctuation is only one character, otherwise it's practically useless
		if (key_to_message.size() != default_messages.size() && (!keys_have_whitespace || punctuation.size() == 1)) {
			Ref<RegEx> re;
			word_regex.instantiate();

			String char_re = "[\\w\\d";
			for (char32_t p : punctuation) {
				char_re += "\\" + String::chr(p);
			}
			char_re += "]";
			if (!keys_have_whitespace) {
				word_regex->compile(common_to_all_prefix + char_re + "+");
			} else {
				word_regex->compile("\\b" + common_to_all_prefix + char_re + "+" + "\\b");
			}

			err = run_stage(&KeyWorker::partial_task, resource_strings, "Stage 2");
			if (err != OK) {
				return pop_keys();
			}
		} else {
			end_stage();
		}

		// Stage 3: commonly known suffixes
		// We first filter them according to common characteristics so that this doesn't take forever.
		if (key_to_message.size() != default_messages.size()) {
			auto filter_things = [&]() {
				filtered_resource_strings.clear();
				for (const String &res_s : res_strings) {
					if (should_filter(res_s)) {
						continue;
					}
					filtered_resource_strings.push_back(res_s);
				}
				return filtered_resource_strings.size();
			};
			filter_things();
			// check if upper case strings are >90% of the strings
			if (filtered_resource_strings.size() > MAX_FILT_RES_STRINGS && (!keys_are_all_upper || !keys_are_all_lower || !keys_are_all_ascii)) {
				if (!keys_are_all_upper && keys_that_are_all_upper / key_to_message.size() > 0.9) {
					// if so, we can safely assume that the keys are all upper case
					keys_are_all_upper = true;
				} else if (!keys_are_all_lower && keys_that_are_all_lower / key_to_message.size() > 0.9) {
					// if so, we can safely assume that the keys are all lower case
					keys_are_all_lower = true;
				}
				if (!keys_are_all_ascii && keys_that_are_all_ascii / key_to_message.size() > 0.9) {
					// if so, we can safely assume that the keys are all ascii
					keys_are_all_ascii = true;
				}
				filter_things();
			}
			// add the message strings to the filtered resource strings
			Vector<String> new_strings;
			get_sanitized_message_strings(new_strings);
			filtered_resource_strings.append_array(new_strings);

			common_prefixes = get_sanitized_strings(STANDARD_SUFFIXES);
			common_suffixes = get_sanitized_strings(STANDARD_SUFFIXES);
			pop_charstr_vectors();
			Error err = run_stage(&KeyWorker::prefix_suffix_task_2, filtered_resource_strings_t, "Stage 3");
			if (err != OK) {
				return pop_keys();
			}
		}
		// Stage 3.5: Try to find keys with numeric suffixes
		if (key_to_message.size() != default_messages.size()) {
			Vector<String> stripped_strings = filtered_resource_strings;
			HashSet<Pair<CharString, int>> stripped_strings_set;
			for (int i = 0; i < stripped_strings.size(); i++) {
				auto &str = stripped_strings[i];
				int num_suffix_val = -1;
				CharString ut = str.utf8();
				ut = try_strip_numeric_suffix(ut, num_suffix_val);
				stripped_strings_set.insert({ ut, num_suffix_val });
			}
			auto vec = gdre::hashset_to_vector(stripped_strings_set);
			Error err = run_stage(&KeyWorker::stage_3_5_task, vec, "Stage 3");
			if (err != OK) {
				return pop_keys();
			}
		}
		// Stage 4: Combine resource strings with detected prefixes and suffixes
		// If we're still missing keys and no keys have spaces, we try combining every string with every other string
		do_stage_4 = do_stage_4 && key_to_message.size() != default_messages.size();
		if (do_stage_4 && key_to_message.size() != default_messages.size()) {
			auto curr_keys = get_keys(key_to_message);
			find_common_prefixes_and_suffixes(curr_keys);

			Vector<String> middle_candidates;
			extract_middles(filtered_resource_strings, middle_candidates);
			Vector<String> str_keys;
			for (const auto &E : key_to_message) {
				str_keys.push_back(get_key(E));
			}
			extract_middles(str_keys, middle_candidates);
			Vector<String> new_strings;
			get_sanitized_message_strings(new_strings);
			middle_candidates.append_array(new_strings);
			middle_candidates = gdre::hashset_to_vector(gdre::vector_to_hashset(middle_candidates));
			auto thingy = gdre::vector_to_hashset(filtered_resource_strings);
			for (auto &middle : middle_candidates) {
				if (thingy.has(middle)) {
					continue;
				}
				filtered_resource_strings.push_back(middle);
			}

			start_of_multithread = OS::get_singleton()->get_ticks_usec();
			pop_charstr_vectors();
			for (const auto &prefix : common_prefixes_t) {
				for (const auto &suffix : common_suffixes_t) {
					if (try_key_suffix(prefix.get_data(), suffix.get_data())) {
						reg_successful_prefix(prefix.get_data());
					}
					try_num_suffix(prefix.get_data(), suffix.get_data());
				}
			}
			if (filtered_resource_strings.size() <= MAX_FILT_RES_STRINGS) {
				Error err = run_stage(&KeyWorker::prefix_suffix_task_2, filtered_resource_strings_t, "Stage 4");
				if (err != OK) {
					return pop_keys();
				}
				// Stage 5: Combine resource strings with every other string
				// If we're still missing keys, we try combining every string with every other string.
				do_stage_5 = do_stage_5 && key_to_message.size() != default_messages.size() && filtered_resource_strings.size() <= MAX_FILT_RES_STRINGS;
				if (do_stage_5) {
					Error err = run_stage(&KeyWorker::stage_5_task_2, filtered_resource_strings_t, "Stage 5");
					if (err != OK) {
						return pop_keys();
					}
				}
			}
		}

		missing_keys = pop_keys();
		// print out the times taken
		bl_debug("Key guessing took " + itos(OS::get_singleton()->get_ticks_msec() - start_time) + "ms");
		for (int i = 0; i < times.size(); i++) {
			auto num_keys = keys_found[i];
			if (i == 0) {
				bl_debug("Stage 1 took " + itos(times[i] - start_time) + "ms, found " + itos(num_keys) + " keys");
			} else {
				bl_debug("Stage " + itos(i + 1) + " took " + itos(times[i] - times[i - 1]) + "ms, found " + itos(num_keys) + " keys");
			}
			if (i >= 2 && num_keys > 0) {
				if (num_keys < 50) {
					for (const auto &key : stages_keys_found[i]) {
						bl_debug("* Key found in stage " + itos(i + 1) + ": " + key);
					}
				} else {
					bl_debug("*** Stage " + itos(i + 1) + " found a LOT keys");
				}
			}
		}
		bl_debug(vformat("Total found: %d/%d", default_messages.size() - missing_keys, default_messages.size()));
		return missing_keys;
	}
};

Ref<ExportReport> TranslationExporter::export_resource(const String &output_dir, Ref<ImportInfo> iinfo) {
	// Implementation for exporting resources related to translations
	Error err = OK;
	// translation files are usually imported from one CSV and converted to multiple "<LOCALE>.translation" files
	// TODO: make this also check for the first file in GDRESettings::get_singleton()->get_project_setting("internationalization/locale/translations")
	const String locale_setting_key = GDRESettings::get_singleton()->get_ver_major() >= 4 ? "internationalization/locale/fallback" : "locale/fallback";
	String default_locale = GDRESettings::get_singleton()->pack_has_project_config() && GDRESettings::get_singleton()->has_project_setting(locale_setting_key)
			? GDRESettings::get_singleton()->get_project_setting(locale_setting_key)
			: "en";
	auto dest_files = iinfo->get_dest_files();
	bool has_default_translation = false;
	if (dest_files.size() > 1) {
		for (const String &path : dest_files) {
			if (path.get_basename().get_extension().to_lower() == default_locale) {
				has_default_translation = true;
				break;
			}
		}
	}
	if (!has_default_translation) {
		default_locale = dest_files[0].get_basename().get_extension().to_lower();
		has_default_translation = !default_locale.is_empty();
	}

	String export_dest = iinfo->get_export_dest();
	String old_translation_csv_path;
	const Vector<String> old_translation_csv_paths = GDRESettings::get_singleton()->get_old_translation_csv_paths();
	if (!old_translation_csv_paths.is_empty()) {
		String export_dest_fname = export_dest.get_file();
		for (const String &path : old_translation_csv_paths) {
			if (path.get_file() == export_dest_fname) {
				old_translation_csv_path = path;
				break;
			}
		}
	}

	bl_debug("Exporting translation file " + export_dest);
	Vector<Ref<Translation>> translations;
	Vector<Vector<String>> translation_messages;
	uint64_t default_messages_index = UINT64_MAX;
	String header = "key";
	Vector<String> keys;
	Ref<ExportReport> report = memnew(ExportReport(iinfo));
	report->set_error(ERR_CANT_ACQUIRE_RESOURCE);
	for (String path : dest_files) {
		Ref<Translation> tr = ResourceCompatLoader::non_global_load(path, "", &err);
		ERR_FAIL_COND_V_MSG(err != OK, report, "Could not load translation file " + iinfo->get_path());
		ERR_FAIL_COND_V_MSG(!tr.is_valid(), report, "Translation file " + iinfo->get_path() + " was not valid");
		String locale = tr->get_locale();
		// TODO: put the default locale at the beginning
		header += "," + locale;
		if (tr->get_class_name() != "OptimizedTranslation") {
			// We have a real translation class, get the keys
			if (keys.size() == 0 && (!has_default_translation || locale.to_lower() == default_locale.to_lower())) {
				List<StringName> key_list;
				tr->get_message_list(&key_list);
				for (auto key : key_list) {
					keys.push_back(key);
				}
			}
		}
		Vector<String> messages = tr->get_translated_message_list();
		if (locale.to_lower() == default_locale.to_lower()) {
			default_messages_index = translation_messages.size();
		}
		translation_messages.push_back(messages);
		translations.push_back(tr);
	}

	if (default_messages_index == UINT64_MAX) {
		if (!has_default_translation) {
			default_messages_index = 0;
		} else {
			report->set_error(ERR_FILE_MISSING_DEPENDENCIES);
			ERR_FAIL_V_MSG(report, "No default translation found for " + iinfo->get_path());
		}
	}
	// check default_messages for empty strings
	size_t empty_strings = 0;
	for (auto &message : translation_messages[default_messages_index]) {
		if (message.is_empty()) {
			empty_strings++;
		}
	}
	// if >20% of the strings are empty, this probably isn't the default translation; search the rest of the translations for a non-empty string
	if (empty_strings > translation_messages[default_messages_index].size() * 0.2) {
		size_t best_empty_strings = empty_strings;
		for (int i = 0; i < translations.size(); i++) {
			size_t empty_strings = 0;
			for (auto &message : translation_messages[i]) {
				if (message.is_empty()) {
					empty_strings++;
				}
			}
			if (empty_strings < best_empty_strings) {
				best_empty_strings = empty_strings;
				default_messages_index = i;
			}
		}
	}

	// remove empty strings
	if (keys.is_empty()) { // optimized
		for (auto &translation_message : translation_messages) {
			for (int64_t i = translation_message.size() - 1; i >= 0; i--) {
				if (translation_message[i].is_empty()) {
					translation_message.remove_at(i);
				}
			}
		}
	} else {
		for (auto &translation_message : translation_messages) {
			for (int64_t i = translation_message.size() - 1; i >= 0; i--) {
				if (translation_message[i].is_empty() && keys[i].is_empty()) {
					translation_message.remove_at(i);
					keys.remove_at(i);
				}
			}
		}
	}

	// We can't recover the keys from Optimized translations, we have to guess
	int missing_keys = 0;
	bool is_optimized = keys.size() == 0;
	if (is_optimized) {
		KeyWorker kw(translations[default_messages_index], translation_messages[default_messages_index], all_keys_found);
		kw.path = iinfo->get_path();
		kw.default_locale = default_locale;
		kw.old_translation_csv_path = old_translation_csv_path;
		missing_keys = kw.run();
		keys = kw.keys;

		// remove duplicate key
		HashSet<String> key_set;
		for (int64_t i = 0; i < keys.size(); i++) {
			const auto &key = keys[i];
			if (!key_set.has(key)) {
				key_set.insert(key);
			} else {
				keys.remove_at(i);
				for (auto &translation_message : translation_messages) {
					translation_message.remove_at(i);
				}
				i--;
			}
		}

		for (auto &key : keys) {
			if (!String(key).begins_with(MISSING_KEY_PREFIX)) {
				all_keys_found.insert(key);
			}
		}
	}
	header += "\n";
	// If greater than 15% of the keys are missing, we save the file to the export directory.
	// The reason for this threshold is that the translations may contain keys that are not currently in use in the project.
	bool resave = missing_keys > (translation_messages[default_messages_index].size() * threshold);
	if (resave) {
		iinfo->set_export_dest("res://.assets/" + iinfo->get_export_dest().replace("res://", ""));
	}
	String output_path = output_dir.simplify_path().path_join(iinfo->get_export_dest().replace("res://", ""));
	err = write_to_csv(output_path, header, keys, translation_messages);
	if (err != OK) {
		report->set_error(err);
		return report;
	}
	if (!old_translation_csv_path.is_empty()) {
		Vector<String> old_translation_csv_keys;
		Vector<String> old_translation_header;
		Vector<String> add_locales;
		HashMap<int64_t, String> old_translation_index_locale_map;
		HashMap<String, Ref<Translation>> old_translation_map;
		{
			Ref<FileAccess> f = FileAccess::open(old_translation_csv_path, FileAccess::READ);
			old_translation_header = f->get_csv_line();
			for (int64_t i = 1; i < old_translation_header.size(); i++) {
				const String &locale = old_translation_header[i];
				if (locale.left(1) == "_" || locale.is_empty()) {
					continue;
				}

				Ref<Translation> translation;
				translation.instantiate();
				translation->set_locale(locale);
				old_translation_map[locale] = translation;

				old_translation_index_locale_map[i] = locale;

				auto it = std::find_if(translations.begin(), translations.end(), [&](const Ref<Translation> &tr) { return tr->get_locale() == locale; });
				if (it == translations.end()) {
					add_locales.append(locale);
				}
			}

			while (f.is_valid() && !f->eof_reached()) {
				Vector<String> line = f->get_csv_line();
				if (line.size() <= 1) {
					continue;
				}
				const String& key = line[0];
				if (key.is_empty()) {
					continue;
				}
				old_translation_csv_keys.append(key);

				for (const auto &p : old_translation_index_locale_map) {
					const auto index = p.key;
					const auto& locale = p.value;
					const auto &tr = old_translation_map[locale];
					tr->add_message(key, line[index].c_unescape());
				}
			}
		}

		if (!old_translation_csv_keys.is_empty()) {
			Vector<String> sorted_keys;
			Vector<Vector<String>> sorted_translation_messages;
			for (int64_t i = 0; i < translation_messages.size(); i++) {
				sorted_translation_messages.push_back(Vector<String>());
			}

			for (const String &key : old_translation_csv_keys) {
				const int64_t idx = keys.find(key);
				sorted_keys.append(key);
				if (idx >= 0) {
					for (int64_t i = 0; i < translation_messages.size(); i++) {
						sorted_translation_messages.write[i].append(translation_messages[i][idx]);
					}

					keys.write[idx] = String();
				} else {
					for (int64_t i = 0; i < translation_messages.size(); i++) {
						sorted_translation_messages.write[i].append("");
					}
				}
			}
			for (int64_t idx = 0; idx < keys.size(); idx++) {
				const auto &key = keys[idx];
				if (key.is_empty()) {
					continue;
				}
				sorted_keys.append(key);

				for (int64_t i = 0; i < translation_messages.size(); i++) {
					sorted_translation_messages.write[i].append(translation_messages[i][idx]);
				}
			}

			// diff_fmt.csv output
			String diff_header = "key";

			for (const auto &tr : translations) {
				String locale = tr->get_locale();
				diff_header += "," + locale;
			}
			for (const auto &locale : add_locales) {
				diff_header += "," + locale;
			}
			diff_header += ",old_" + default_locale;
			diff_header += ",is_add_" + default_locale;
			diff_header += ",is_update_" + default_locale;
			diff_header += ",is_remove_" + default_locale;
			diff_header += "\n";

			Vector<Vector<String>> add_locale_column;
			Vector<String> old_default_locale_column;
			Vector<String> is_add_column;
			Vector<String> is_update_column;
			Vector<String> is_remove_column;

			for (int64_t i = 0; i < add_locales.size(); i++) {
				add_locale_column.push_back(Vector<String>());
			}

			for (int64_t i = 0; i < sorted_keys.size(); i++) {
				const auto &key = sorted_keys[i];

				for (int64_t j = 0; j < add_locales.size(); j++) {
					const auto &locale = add_locales[j];
					const auto &tr = old_translation_map[locale];
					const auto &add_message = tr->get_message(key).operator String();
					add_locale_column.write[j].append(add_message);
				}

				{
					const auto &tr = old_translation_map[default_locale];
					const auto &add_message = tr->get_message(key).operator String();
					old_default_locale_column.append(add_message);
				}

				{
					const auto &new_tr = translations[default_messages_index];
					const auto &old_tr = old_translation_map[default_locale];

					const auto &new_message = new_tr->get_message(key).operator String();
					const auto &old_message = old_tr->get_message(key).operator String();

					String is_add = "";
					if (!new_message.is_empty() && old_message.is_empty()) {
						is_add = "1";
					}
					is_add_column.append(is_add);

					String is_update = "";
					if (!new_message.is_empty() && !old_message.is_empty() && new_message != old_message) {
						is_update = "1";
					}
					is_update_column.append(is_update);

					String is_remove = "";
					if (new_message.is_empty() && !old_message.is_empty()) {
						is_remove = "1";
					}
					is_remove_column.append(is_remove);
				}
			}

			sorted_translation_messages.append_array(add_locale_column);
			sorted_translation_messages.append(old_default_locale_column);
			sorted_translation_messages.append(is_add_column);
			sorted_translation_messages.append(is_update_column);
			sorted_translation_messages.append(is_remove_column);

			String export_dest_dir = iinfo->get_export_dest().get_base_dir().replace("res://", "");
			String export_dest_fname = iinfo->get_export_dest().get_file().get_basename() + "_diff_fmt.csv";

			String output_path = output_dir.simplify_path().path_join(export_dest_dir).path_join(export_dest_fname);
			err = write_to_csv(output_path, diff_header, sorted_keys, sorted_translation_messages);
			if (err != OK) {
				report->set_error(err);
				return report;
			}
		}
	}
	report->set_error(OK);
	Dictionary extra_info;
	extra_info["missing_keys"] = missing_keys;
	extra_info["total_keys"] = translation_messages[default_messages_index].size();
	report->set_extra_info(extra_info);
	if (missing_keys) {
		String translation_export_message = "WARNING: Could not recover " + itos(missing_keys) + " keys for " + iinfo->get_source_file() + "\n";
		if (resave) {
			translation_export_message += "Saved " + iinfo->get_source_file().get_file() + " to " + iinfo->get_export_dest() + "\n";
		}
		report->set_message(translation_export_message);
	}
	if (iinfo->get_ver_major() >= 4) {
		iinfo->set_param("compress", is_optimized);
		iinfo->set_param("delimiter", 0);
	}
	report->set_new_source_path(iinfo->get_export_dest());
	report->set_saved_path(output_path);
	return report;
}

void TranslationExporter::get_handled_types(List<String> *out) const {
	// Add the types of resources that this exporter can handle
	out->push_back("Translation");
	out->push_back("PHashTranslation");
	out->push_back("OptimizedTranslation");
}

void TranslationExporter::get_handled_importers(List<String> *out) const {
	// Add the importers that this exporter can handle
	out->push_back("csv_translation");
	out->push_back("translation_csv");
	out->push_back("translation");
}

String TranslationExporter::get_name() const {
	return "Translation";
}

String TranslationExporter::get_default_export_extension(const String &res_path) const {
	return "csv";
}
