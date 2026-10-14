// 静态文件服务的纯函数测试：路径解析（穿越防护）与 MIME 判定
//（行为与退役的 python 网关 web/serve.py 对齐）
#include <gtest/gtest.h>

#include "web_static.h"

TEST(WebStatic, ResolvesNormalPaths) {
    EXPECT_EQ(fg::resolve_static_path("/"), "index.html");
    EXPECT_EQ(fg::resolve_static_path(""), "index.html");
    EXPECT_EQ(fg::resolve_static_path("//"), "index.html");
    EXPECT_EQ(fg::resolve_static_path("/index.html"), "index.html");
    EXPECT_EQ(fg::resolve_static_path("/js/app.js"), "js/app.js");
    EXPECT_EQ(fg::resolve_static_path("js/app.js"), "js/app.js");
    EXPECT_EQ(fg::resolve_static_path("/js//app.js"), "js/app.js");      // 重复斜杠
    EXPECT_EQ(fg::resolve_static_path("/js/./app.js"), "js/app.js");     // "." 段
    EXPECT_EQ(fg::resolve_static_path("/style.css?v=3#x"), "style.css");  // query/fragment 剥掉
}

TEST(WebStatic, RejectsTraversal) {
    // 目录穿越一律拒绝（直接出现与藏在中段的 ".."）
    EXPECT_TRUE(fg::resolve_static_path("/../secret").empty());
    EXPECT_TRUE(fg::resolve_static_path("/js/../secret").empty());
    EXPECT_TRUE(fg::resolve_static_path("/a/../../b").empty());
    EXPECT_TRUE(fg::resolve_static_path("/..").empty());
    // 反斜杠（Windows 下也是分隔符）出现即拒绝
    EXPECT_TRUE(fg::resolve_static_path("/js\\..\\secret").empty());
    // URL 编码穿越不做解码：按字面查文件自然 404——解析结果只是普通文件名
    EXPECT_EQ(fg::resolve_static_path("/%2e%2e/secret"), "%2e%2e/secret");
}

TEST(WebStatic, MimeTypes) {
    EXPECT_STREQ(fg::mime_type_for("index.html"), "text/html; charset=utf-8");
    EXPECT_STREQ(fg::mime_type_for("js/app.js"), "text/javascript; charset=utf-8");
    EXPECT_STREQ(fg::mime_type_for("style.css"), "text/css; charset=utf-8");
    EXPECT_STREQ(fg::mime_type_for("data.json"), "application/json; charset=utf-8");
    EXPECT_STREQ(fg::mime_type_for("icon.png"), "image/png");
    EXPECT_STREQ(fg::mime_type_for("favicon.ico"), "image/x-icon");
    EXPECT_STREQ(fg::mime_type_for("logo.svg"), "image/svg+xml");
    // 大小写不敏感（serve.py 用 suffix.lower()）
    EXPECT_STREQ(fg::mime_type_for("APP.JS"), "text/javascript; charset=utf-8");
    // 未知/无扩展名回二进制流（与 serve.py 一致）
    EXPECT_STREQ(fg::mime_type_for("weird.bin"), "application/octet-stream");
    EXPECT_STREQ(fg::mime_type_for("noext"), "application/octet-stream");
}
