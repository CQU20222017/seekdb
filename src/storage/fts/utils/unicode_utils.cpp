/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/storage/fts/utils/unicode_utils.h"

#include "lib/charset/ob_charset_string_helper.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace storage
{
// See: java's Character.UnicodeBlock

bool ObUnicodeBlockUtils::is_alpha(const ob_wc_t unicode)
{
  bool is_ascii_alpha
      = (unicode >= 0x41 && unicode <= 0x5a) || (unicode >= 0x61 && unicode <= 0x7a);
  bool is_fw_alpha
      = (unicode >= 0xFF21 && unicode <= 0xFF3A) || (unicode >= 0xFF41 && unicode <= 0xFF5A);
  return is_ascii_alpha || is_fw_alpha;
}

bool ObUnicodeBlockUtils::is_arabic(const ob_wc_t unicode)
{
  bool is_ascii_arabic = (unicode >= 0x30 && unicode <= 0x39);
  bool is_fw_arabic = (unicode >= 0xFF10 && unicode <= 0xFF19);
  return is_ascii_arabic || is_fw_arabic;
}

bool ObUnicodeBlockUtils::check_high_surrogate(const ob_wc_t unicode)
{
  return unicode >= 0xD800 && unicode <= 0xDBFF;
}

bool ObUnicodeBlockUtils::check_high_private_use_surrogate(const ob_wc_t unicode)
{
  return unicode >= 0xDB80 && unicode <= 0xDBFF;
}

bool ObUnicodeBlockUtils::check_low_surrogate(const ob_wc_t unicode)
{
  return unicode >= 0xDC00 && unicode <= 0xDFFF;
}

bool ObUnicodeBlockUtils::check_letter_connector(const ob_wc_t unicode)
{
  static constexpr char LETTER_CONNECTOR[] = {'#', '&', '+', '-', '.', '@', '_'};

  ob_wc_t code = unicode;
  if (code > 0xFF00 && code < 0xFF5F) {
    code -= 0xFEE0;
  }
  for (int i = 0; i < ARRAYSIZEOF(LETTER_CONNECTOR); i++) {
    if (code == LETTER_CONNECTOR[i]) {
      return true;
    }
  }
  return false;
}

bool ObUnicodeBlockUtils::check_num_connector(const ob_wc_t unicode)
{
  static constexpr char NUM_CONNECTOR[] = {',', '.'};
  ob_wc_t code = unicode;
  if (code > 0xFF00 && code < 0xFF5F) {
    code -= 0xFEE0;
  }
  for (int i = 0; i < ARRAYSIZEOF(NUM_CONNECTOR); i++) {
    if (code == NUM_CONNECTOR[i]) {
      return true;
    }
  }
  return false;
}

bool ObUnicodeBlockUtils::check_ignore_as_single(const ob_wc_t unicode)
{
  // cjk to ascii part
  if (unicode > 0xFF00 && unicode < 0xFF5F) {
    // if output as a single char, it will be ignored
    if (!is_alpha(unicode) && !is_arabic(unicode)) {
      return true;
    }
  }
  return false;
}

bool ObUnicodeBlockUtils::is_chinese(const ob_wc_t unicode)
{
  // variables name keep same with it's code block name, skip code style.
  bool CJK_UNIFIED_IDEOGRAPHS = (unicode >= 0x4E00 && unicode <= 0x9FFF);
  bool CJK_COMPATIBILITY_IDEOGRAPHS = (unicode >= 0xF900 && unicode <= 0xFAFF);
  bool CJK_UNIFIED_IDEOGRAPHS_EXTENSION_A = (unicode >= 0x3400 && unicode <= 0x4DBF);
  return CJK_UNIFIED_IDEOGRAPHS || CJK_COMPATIBILITY_IDEOGRAPHS
         || CJK_UNIFIED_IDEOGRAPHS_EXTENSION_A;
}

bool ObUnicodeBlockUtils::is_other_cjk(const ob_wc_t unicode)
{
  // variables name keep same with it's code block name, skip code style.
  bool HALFWIDTH_AND_FULLWIDTH_FORMS = (unicode >= 0xFF00 && unicode <= 0xFFEF);

  bool HANGUL_SYLLABLES = (unicode >= 0xAC00 && unicode <= 0xD7AF);
  bool HANGUL_JAMO = (unicode >= 0x1100 && unicode <= 0x11FF);
  bool HANGUL_COMPATIBILITY_JAMO = (unicode >= 0x3130 && unicode <= 0x318f);

  bool HIRAGANA = (unicode >= 0x3040 && unicode <= 0x309F);
  bool KATAKANA = (unicode >= 0x30A0 && unicode <= 0x30FF);
  bool KATAKANA_PHONETIC_EXTENSIONS = (unicode >= 0x31F0 && unicode <= 0x31FF);

  return HALFWIDTH_AND_FULLWIDTH_FORMS || HANGUL_SYLLABLES || HANGUL_JAMO
         || HANGUL_COMPATIBILITY_JAMO || HIRAGANA || KATAKANA || KATAKANA_PHONETIC_EXTENSIONS;
}

bool ObUnicodeBlockUtils::is_surrogate(const ob_wc_t unicode)
{
  bool HIGH_SURROGATES = (unicode >= 0xD800 && unicode <= 0xDB7F);
  bool LOW_SURROGATES = (unicode >= 0xDC00 && unicode <= 0xDFFF);

  return (HIGH_SURROGATES || LOW_SURROGATES);
}

bool ObUnicodeBlockUtils::is_convertable_fullwidth(const ob_wc_t unicode)
{
  bool FW_SPACE = (unicode == 0x3000);
  bool FW_OHTER = (unicode > 0xFF00 && unicode < 0xFF5F);
  return FW_SPACE || FW_OHTER;
}

bool ObUnicodeBlockUtils::is_unicode_cn_number(ob_wc_t unicode)
{
  // Chinese numerical characters: "一二两三四五六七八九十零壹贰叁肆伍陆柒捌玖拾百千万亿佰仟萬億兆卅廿"
  //
  // This predicate is evaluated for every CJK character on the tokenizer hot
  // path, so the set is kept as a sorted table of code points and looked up
  // with a binary search. The membership set is the same as before (the old
  // table held one duplicate entry), so the result is unchanged.
  static const ob_wc_t cn_number_array[] = {
      0x4E00, 0x4E03, 0x4E07, 0x4E09, 0x4E24, 0x4E5D, 0x4E8C, 0x4E94,
      0x4EBF, 0x4EDF, 0x4F0D, 0x4F70, 0x5104, 0x5146, 0x516B, 0x516D,
      0x5341, 0x5343, 0x5345, 0x53C1, 0x56DB, 0x58F9, 0x5EFF, 0x62FE,
      0x634C, 0x67D2, 0x7396, 0x767E, 0x8086, 0x842C, 0x8D30, 0x9646,
      0x96F6,
  };
  const int array_size = static_cast<int>(sizeof(cn_number_array) / sizeof(cn_number_array[0]));
  int low = 0;
  int high = array_size - 1;
  while (low <= high) {
    const int mid = low + ((high - low) >> 1);
    if (unicode == cn_number_array[mid]) {
      return true;
    } else if (unicode < cn_number_array[mid]) {
      high = mid - 1;
    } else {
      low = mid + 1;
    }
  }
  return false;
}

} //  namespace storage
} //  namespace oceanbase
