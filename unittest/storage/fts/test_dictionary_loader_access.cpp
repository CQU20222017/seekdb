/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "lib/charset/ob_charset.h"
#include "storage/fts/dict/ob_gen_dic_loader.h"

namespace oceanbase {
namespace storage {
namespace {
using namespace common;

class DictionaryLoaderAccessTest : public testing::Test
{
protected:
  ObGenDicLoader &registry = ObGenDicLoader::get_instance();
  void SetUp() override
  {
    const int ret = registry.init();
    ASSERT_TRUE(ret == OB_SUCCESS || ret == OB_INIT_TWICE);
  }
};

TEST_F(DictionaryLoaderAccessTest, concurrent_first_lookup_and_repeated_hits)
{
  constexpr int threads = 12;
  const char *keys[] = {"ik.read-race-a", "ik.read-race-b"};
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<ObDicLoader *> identities(threads, nullptr);
  std::vector<int> errors(threads, OB_SUCCESS);
  std::vector<std::thread> workers;
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&, i] {
      ready.fetch_add(1);
      while (!start.load()) { std::this_thread::yield(); }
      for (int j = 0; j < 500; ++j) {
        ObDicLoaderHandle handle;
        const int ret = registry.get_dic_loader(
            ObString::make_string(keys[i % 2]), CHARSET_UTF8MB4, handle);
        if (ret != OB_SUCCESS || !handle.is_valid()) {
          errors[i] = ret == OB_SUCCESS ? OB_ERR_UNEXPECTED : ret;
          break;
        }
        ObDicLoader *loader = handle.get_loader();
        if (loader->get_dic_tables_info().count() != 3
            || (identities[i] != nullptr && identities[i] != loader)) {
          errors[i] = OB_ERR_UNEXPECTED;
          break;
        }
        identities[i] = loader;
      }
    });
  }
  while (ready.load() != threads) { std::this_thread::yield(); }
  start.store(true);
  for (auto &worker : workers) { worker.join(); }
  for (int i = 0; i < threads; ++i) {
    EXPECT_EQ(OB_SUCCESS, errors[i]);
    EXPECT_NE(nullptr, identities[i]);
    EXPECT_EQ(identities[i % 2], identities[i]);
  }
  EXPECT_NE(identities[0], identities[1]);
}

TEST_F(DictionaryLoaderAccessTest, handles_retain_one_shared_registry_loader)
{
  ObDicLoaderHandle first;
  const ObString key = ObString::make_string("ik.read-handles");
  ASSERT_EQ(OB_SUCCESS, registry.get_dic_loader(key, CHARSET_UTF8MB4, first));
  ObDicLoader *identity = first.get_loader();
  ASSERT_NE(nullptr, identity);
  EXPECT_EQ(3, identity->inc_ref()); // Registry, handle, and this temporary reference.
  EXPECT_EQ(2, identity->dec_ref());
  ObDicLoaderHandle copy(first);
  first.reset();
  ASSERT_EQ(OB_SUCCESS, registry.get_dic_loader(key, CHARSET_UTF8MB4, first));
  EXPECT_EQ(identity, first.get_loader());
  EXPECT_EQ(identity, copy.get_loader());
  copy.reset();
  first.reset();
  ASSERT_EQ(OB_SUCCESS, registry.get_dic_loader(key, CHARSET_UTF8MB4, first));
  EXPECT_EQ(identity, first.get_loader());
  EXPECT_EQ(3, identity->inc_ref());
  EXPECT_EQ(2, identity->dec_ref());
}

TEST_F(DictionaryLoaderAccessTest, invalid_requests_preserve_existing_handle)
{
  ObDicLoaderHandle handle;
  ASSERT_EQ(OB_SUCCESS, registry.get_dic_loader(
      ObString::make_string("ik.read-validation"), CHARSET_UTF8MB4, handle));
  ObDicLoader *identity = handle.get_loader();
  EXPECT_EQ(OB_INVALID_ARGUMENT, registry.get_dic_loader(ObString(), CHARSET_UTF8MB4, handle));
  EXPECT_EQ(OB_INVALID_ARGUMENT, registry.get_dic_loader(
      ObString::make_string("ik"), CHARSET_INVALID, handle));
  std::string long_name(OB_FT_PARSER_NAME_LENGTH, 'i');
  EXPECT_EQ(OB_INVALID_ARGUMENT, registry.get_dic_loader(
      ObString::make_string(long_name.c_str()), CHARSET_UTF8MB4, handle));
  EXPECT_EQ(OB_NOT_SUPPORTED, registry.get_dic_loader(
      ObString::make_string("unknown-parser"), CHARSET_UTF8MB4, handle));
  EXPECT_EQ(OB_NOT_SUPPORTED, registry.get_dic_loader(
      ObString::make_string("ik"), CHARSET_UTF16, handle));
  EXPECT_EQ(identity, handle.get_loader());
}
} // namespace
} // namespace storage
} // namespace oceanbase
