#pragma once
#include "common/datum/ob_datum.h"
#include "share/datum/ob_datum_funcs.h"

namespace oceanbase { namespace storage {
// CS_TYPE_BINARY uses an unpadded byte comparison. Fixed-size document IDs
// can compare native words without the generic collation dispatch. Other
// lengths retain bytewise prefix/length ordering; exceptional datum forms
// retain the registered comparator and its error behavior.
OB_INLINE int compare_binary_document_ids(
    const common::ObDatum &left, const common::ObDatum &right, int &result,
    const common::ObDatumAccessContext *access_ctx)
{
  if (left.is_ext() || right.is_ext() || left.is_outrow() || right.is_outrow()) {
    return common::ObDatumFuncs::get_basic_func(common::ObVarcharType, common::CS_TYPE_BINARY)
        ->null_first_cmp_(left, right, result, access_ctx);
  }
  if (left.is_null() || right.is_null()) {
    result = left.is_null() ? (right.is_null() ? 0 : -1) : 1;
  } else if (left.len_ == right.len_ && (left.len_ == 8 || left.len_ == 16)) {
    uint64_t a = 0, b = 0;
    MEMCPY(&a, left.ptr_, sizeof(a));
    MEMCPY(&b, right.ptr_, sizeof(b));
    if (a == b && left.len_ == 16) {
      MEMCPY(&a, left.ptr_ + sizeof(a), sizeof(a));
      MEMCPY(&b, right.ptr_ + sizeof(b), sizeof(b));
    }
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    a = __builtin_bswap64(a);
    b = __builtin_bswap64(b);
#endif
    result = (a > b) - (a < b);
  } else {
    const int64_t length = MIN(left.len_, right.len_);
    result = length == 0 ? 0 : MEMCMP(left.ptr_, right.ptr_, length);
    if (result == 0) result = (left.len_ > right.len_) - (left.len_ < right.len_);
  }
  return common::OB_SUCCESS;
}
} }
