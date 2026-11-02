#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Targeted replay of the FTS-sensitive assertions from
tools/deploy/mysql_test/test_suite/fts_index/{cn_word,tokenize_function} with the exact
expected values taken from the checked-in .result files.

The generic runner in tools/competition cannot drive this suite (it does not support
"result_format: 4" or symbolic --error tokens), so this script checks the values directly.
"""
import sys

import pymysql

CREATE = """
create table cn_word_table(
    title VARCHAR(256),
    text  VARCHAR(256),
    note  VARCHAR(256)
)
"""

ROWS = [
    ('晨曦中的问候', '在每一个新的早晨，当第一缕阳光洒进窗户，一句温暖的“你好”可以为新的一天定下积极的基调。无论是对家人、同事还是陌生人，一句简单的问候都能够传递出关怀与尊重，让对方感受到一天的美好从这里开始。', '日常礼仪'),
    ('感恩的心', '当我们接受他人的帮助或是经历了生活中的点滴幸运时，“谢谢”这两个字不仅仅是礼貌性的回应，更是我们内心感激之情的真实写照。表达感谢不仅能够加深人与人之间的联系，还能让我们自己更加珍惜生活中的一切美好。', '情感表达'),
    ('车站的告别', '火车站是一个充满故事的地方，人们在这里相遇又分离。每一次的告别都带着不同的感情，有的是短暂离别后的期待重逢，有的则是远行求学或工作的无奈。无论哪种情况，“再见”不仅仅意味着分别，它也蕴含着对未来的祝福和对再次相见的盼望。', '场景描写'),
    ('友谊长存', '朋友是生命中不可或缺的一部分，他们陪伴我们度过快乐时光，也在困难时刻伸出援手。真正的友谊不会因为时间和距离而褪色，相反，它会在岁月的洗礼下变得更加坚固。无论是在一起欢笑还是共同面对挑战，朋友间的默契和支持都是无比珍贵的。', '人际关系'),
    ('求知若渴', '学习是一场永无止境的旅程，对于知识的渴望驱使着人们不断探索未知的世界。无论是通过书本、网络课程还是实践操作，每一个新学到的知识点都能为我们打开一扇通往更广阔天地的大门。这种追求不仅丰富了个人的精神世界，也为社会的进步贡献了一份力量。', '教育学习'),
    ('幸福时刻', '每个人对于幸福的定义都不尽相同，它可以是一顿丰盛的家庭晚餐，也可以是一次说走就走的旅行。重要的是，在这些看似平凡的瞬间里，我们能够感受到内心的平静与满足。分享这些时刻，不仅能让自己的生活更加多彩，也能将这份喜悦传递给身边的人。', '生活感悟'),
    ('和谐世界', '和平与发展是当今世界的主题，但局部冲突依然存在。维护世界和平需要各国人民共同努力，以对话代替对抗，用合作促进共赢。一个和谐的世界不仅是人类共同的愿望，也是实现可持续发展的必要条件。让我们携手构建一个人类命运共同体，共同创造更加美好的未来。', '社会议题'),
    ('自然之美', '大自然以其无穷的魅力吸引着无数人为之倾倒。从雄伟壮观的山脉到广袤无垠的大海，从四季变换的森林到繁星闪烁的夜空，每一处风景都是大自然的艺术品。走进自然，不仅能让人放松身心，更能激发人们对环境保护的责任感。', '环境描写'),
    ('追梦之旅', '每个人心中都有一个梦想，它是前进的动力，也是生活的指南针。为了实现梦想，有些人选择了离开家乡去远方打拼；有些人则在熟悉的土地上默默耕耘。无论选择怎样的道路，追逐梦想的过程总是充满了挑战与机遇，而最终的成就也将证明一切努力都是值得的。', '个人成长'),
    ('不懈奋斗', '成功从来不是一蹴而就的事情，它背后往往是无数次的尝试与失败。那些站在领奖台上的辉煌时刻，背后隐藏着无数个日夜的辛勤付出。每一个成功的背后，都有着不为人知的故事，它们告诉我们：只要坚持不懈地努力，就没有克服不了的困难。', '励志故事'),
    ('国际交流', '在全球化的今天，英语作为国际语言的重要性日益凸显。无论是商务洽谈还是学术交流，良好的英语沟通能力都是一项重要的技能。例如，一封正式的邮件地址如可以成为跨文化交流的重要桥梁。', '语言应用'),
    ('数字时代的便利', '随着科技的发展，数字已经渗透到我们生活的方方面面。从在线支付到智能家居，从社交媒体到电子商务，数字技术让我们的生活变得更加便捷高效。据统计，全球有超过4亿用户每天使用移动支付服务，这极大地改变了人们的消费习惯。', '科技发展'),
    ('编程入门', '学习编程不仅可以培养逻辑思维，还可以开启无限可能的职业生涯。Python作为一种易于学习且功能强大的编程语言，受到了很多初学者的喜爱。比如，代码片段`print("Hello-World!")`就是每个程序员编写的第一行代码，标志着他们踏上了编程之旅。', '技术教育'),
    ('电子邮件礼仪', '在现代职场中，正确撰写电子邮件是一项基本技能。一封专业的邮件应该包括清晰的主题、得体的称呼、简明扼要的内容以及恰当的结束语。例如，发送给客户的邮件可以这样结尾：“Best regards, [Your Name]”。此外，确保邮件地址格式正确（如）也是很重要的。', '职场技能'),
    ('统计数据的力量', '数据可以帮助我们更好地理解世界。例如，在一项关于城市交通的研究中，研究人员收集了近五年来每天的车辆通行数量，总数达到了惊人的1.2亿辆次。通过分析这些数据，政府可以制定更合理的交通规划，提高城市的运行效率。', '数据应用'),
]

MATCH_CASES = [
    ("MATCH(title) AGAINST ('电子邮件' IN BOOLEAN MODE)", 'title', ['电子邮件礼仪']),
    ("MATCH(title) AGAINST ('邮件' IN BOOLEAN MODE)", 'title', ['电子邮件礼仪']),
    ("MATCH(text) AGAINST ('工作' IN BOOLEAN MODE)", 'title', ['车站的告别']),
    ("MATCH(text) AGAINST ('说走就走' IN BOOLEAN MODE)", 'title', ['幸福时刻']),
    ("MATCH(note) AGAINST ('个人成长' IN BOOLEAN MODE)", 'title', ['追梦之旅']),
    ("MATCH(note) AGAINST ('个人' IN BOOLEAN MODE)", 'title', []),
    ("MATCH(text) AGAINST ('Python' IN BOOLEAN MODE)", 'title', ['编程入门']),
    ("MATCH(text) AGAINST ('python' IN BOOLEAN MODE)", 'title', ['编程入门']),
    ("MATCH(text) AGAINST ('pyTHON' IN BOOLEAN MODE)", 'title', ['编程入门']),
    ("MATCH(text) AGAINST ('1.2亿' IN BOOLEAN MODE)", 'title', ['统计数据的力量']),
    ("MATCH(text) AGAINST ('hello-world')", 'title', ['编程入门']),
]

TOKENIZE_TITLES = [
    '["问候", "中", "晨曦", "的"]',
    '["心", "感恩", "的"]',
    '["车站", "告别", "的"]',
    '["长存", "友谊"]',
    '["求知若渴"]',
    '["时刻", "幸福"]',
    '["世界", "和谐"]',
    '["自然", "之", "美"]',
    '["追", "梦", "之旅"]',
    '["不懈", "奋斗"]',
    '["国际交流"]',
    '["时代", "数字", "的", "便利"]',
    '["编程", "入门"]',
    '["礼仪", "电子邮件"]',
    '["力量", "的", "统计数据"]',
]

SCALAR_CASES = [
    ("SELECT tokenize('OceanBase是一款非常稳定数据库，在110个星球上都非常著名，这简直难以置信', 'ik')",
     '["这简直", "难以置信", "著名", "非常", "在", "oceanbase", "稳定", "110个", "上", "是", '
     '"数据库", "星球", "一款", "都"]'),
    ("SELECT tokenize('人际关系', 'ik')", '["人际关系"]'),
    ("SELECT tokenize('中华人民共和国人民大会堂', 'ik', '[{\"additional_args\": [{\"ik_mode\": \"smart\"}]}]')",
     '["人民大会堂", "中华人民共和国"]'),
    ("SELECT tokenize('中华人民共和国人民大会堂', 'ik', '[{\"additional_args\": [{\"ik_mode\": \"max_word\"}]}]')",
     '["大会", "人民大会堂", "大会堂", "共和国", "人民大会", "人民共和国", "中华人民", "共和", "会堂", '
     '"中华", "中华人民共和国", "国人", "华人", "人民"]'),
    ('SELECT tokenize("一二三四五 六七八", "ngram2", \'[{"additional_args": [{"min_ngram_size": 2}, '
     '{"max_ngram_size": 4}]}]\')',
     '["二三四五", "一二三", "三四", "三四五", "七八", "六七八", "六七", "二三", "一二", "四五", '
     '"一二三四", "二三四"]'),
]

ERROR_CASES = [
    ("SELECT tokenize('hello world', '')", 'Invalid argument'),
    ("SELECT tokenize('hello world', ' ')", 'Invalid argument'),
    ("SELECT tokenize('hello world', 'ng ram')", 'Function ng ram is not defined'),
    ("SELECT tokenize('hello world', 'ngramm', '[{\"output\": \"default\"}]')",
     'Function ngramm is not defined'),
    ("SELECT tokenize('hello world', 'ngram', '[{\"output\": \"defaults\"}]')",
     'output mode should be string default or all'),
    ("SELECT tokenize('hello world', 'ngram', '[{\"undefined\": \"all\"}]')", 'config: undefined'),
    ("SELECT tokenize('hello world', 'ngram', '[{\"case\": \"upper\"}]')", 'case indentifier not supported'),
    ("SELECT tokenize('hello world', 'ngram', '[{\"stopwords\": \"abc,def\"}]')", 'stopwords not supported'),
    ("SELECT tokenize('hello world', 'ngram', '[{\"output\": \"default\"]')", 'Invalid JSON text in argument'),
    ("SELECT tokenize('hello world abcdefghijklmnopqrstuvwxyz', 'ngram', "
     "'[{\"output\": \"all\"}, {\"additional_args\": [{\"ngram_token_size\": 0}]}]')",
     'ngram_token_size must be in [1, 10]'),
    ("SELECT tokenize('hello world abcdefghijklmnopqrstuvwxyz', 'space', "
     "'[{\"output\": \"all\"}, {\"additional_args\": [{\"min_token_size\": 0}]}]')",
     'min_token_size must be in [1, 16]'),
    ("SELECT tokenize('hello world abcdefghijklmnopqrstuvwxyz', 'space', "
     "'[{\"output\": \"all\"}, {\"additional_args\": [{\"not_existed_name\": 11}, "
     "{\"max_token_size\": 10}]}]')", 'space config not supported'),
    ("SELECT tokenize('hello world abcdefghijklmnopqrstuvwxyz', 'ik', "
     "'[{\"output\": \"all\"}, {\"additional_args\": [{\"ngram_token_size\": 6}, "
     "{\"max_token_size\": 10}]}]')", 'ik config not supported'),
]


def main():
    conn = pymysql.connect(host="127.0.0.1", port=2881, user="root", password="",
                           charset="utf8mb4", autocommit=True, database="test")
    cur = conn.cursor()
    cur.execute("SET NAMES utf8mb4")
    cur.execute("SET ob_query_timeout = 100000000000")
    failures = []

    def check(name, got, want):
        if got != want:
            failures.append(name)
            print("FAIL %s\n  got : %r\n  want: %r" % (name, got, want))

    cur.execute("DROP TABLE IF EXISTS cn_word_table")
    cur.execute(CREATE)
    try:
        cur.execute('CREATE FULLTEXT INDEX fts_ik_title on cn_word_table(title) WITH PARSER ik '
                    'PARSER_PROPERTIES=(ik_mode="not_smart")')
        failures.append('not_smart must be rejected')
        print("FAIL not_smart index was accepted")
    except Exception:  # noqa: BLE001
        print("OK   ik_mode=not_smart rejected")
    cur.execute('CREATE FULLTEXT INDEX fts_ik_title on cn_word_table(title) WITH PARSER ik '
                'PARSER_PROPERTIES=(ik_mode="max_word")')
    cur.execute('CREATE FULLTEXT INDEX fts_ik_text on cn_word_table(text) WITH PARSER ik '
                'PARSER_PROPERTIES=(ik_mode="smart")')
    cur.execute('CREATE FULLTEXT INDEX fts_ik_note on cn_word_table(note) WITH PARSER ik')
    for row in ROWS:
        cur.execute("INSERT INTO cn_word_table (title, text, note) VALUES (%s, %s, %s)", row)

    for where, col, want in MATCH_CASES:
        cur.execute("select %s from cn_word_table WHERE %s" % (col, where))
        got = [r[0] for r in cur.fetchall()]
        check("MATCH " + where, got, want)

    cur.execute("select tokenize(title, 'ik') as title from cn_word_table")
    got = [r[0] for r in cur.fetchall()]
    check("tokenize(title) for all rows", got, TOKENIZE_TITLES)

    for sql, want in SCALAR_CASES:
        cur.execute(sql)
        check(sql[:60], cur.fetchone()[0], want)

    for sql, want_msg in ERROR_CASES:
        try:
            cur.execute(sql)
            failures.append("expected error: " + sql[:60])
            print("FAIL expected error but got %r for %s" % (cur.fetchone(), sql[:60]))
        except Exception as exc:  # noqa: BLE001
            msg = str(exc)
            if want_msg not in msg:
                failures.append("wrong error: " + sql[:60])
                print("FAIL error mismatch %s\n  got : %s\n  want contains: %s"
                      % (sql[:60], msg, want_msg))
            else:
                print("OK   error case: %s" % want_msg)

    cur.execute("DROP TABLE IF EXISTS cn_word_table")
    conn.close()
    print("\nRESULT: %s (%d failure(s))" % ("PASS" if not failures else "FAIL", len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())