#pragma once
// 客户端应用：主菜单 / 单机人机配置 / 联机大厅 / 对战 四个界面。
// 单机模式直接驱动 core 的 Room（与服务器同一套权威逻辑）；
// 联机模式通过 NetClient 收发协议消息，在本地重建炮弹轨迹。

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <raylib.h>

#include "net_client.h"
#include "protocol.h"
#include "room.h"
#include "world.h"

namespace fg {

// 简易文本输入框
struct TextBox {
    std::string text;
    size_t max_len = 160;
    bool focused = false;
    bool digits_only = false;     // 只允许数字/小数/负号（参数范围）
    bool allow_e = true;          // digits_only 下是否允许科学计数法 'e'（角度框不适用）
    bool expr_chars_only = false; // 只允许数学表达式字符（与服务器白名单一致）
    // 光标（字节偏移，恒落在 UTF-8 字符边界上）：左右键、插入、退格/Delete 都按字符走。
    // 失焦期间由 handle_input 归位到尾部，重新聚焦时从文本末尾继续输入
    size_t cursor = 0;
    // 选择的固定端（另一个端点是 cursor）：anchor == cursor 表示没有选择。
    // 目前选择来自 Ctrl+A 全选；快捷键与高亮都按这条不变量处理
    size_t anchor = 0;

    void handle_input();
};

class App {
public:
    int run();                                    // 正常窗口
    int run_headless(int games);                  // --selftest：本地人机 N 局
    int run_screenshot(const std::string& path);  // --screenshot：自动对局并截图

private:
    enum class Screen { Menu, LocalSetup, Lobby, CreateRoom, Battle };

    // 主循环一帧
    void update();
    void draw();

    // 各界面
    void update_menu();
    void update_local_setup();
    void update_lobby();
    void update_create_room();
    void update_battle();
    void draw_menu();
    void draw_local_setup();
    void draw_lobby();
    void draw_create_room();
    void draw_battle();
    // 联机：房间内"成员 + 下一把配置"面板（房主可改，其他人只读）
    void draw_room_area(const Rectangle& left, const Rectangle& right);

    // UI 小工具
    bool ui_button(Rectangle r, const std::string& label, bool selected = false,
                   bool enabled = true);
    // enabled=false 的输入框不可点击聚焦（点击不消费，等同点在空白处）；
    // 已聚焦语义不受影响（输入框只在获得焦点后吃键）
    void ui_textbox(TextBox& box, Rectangle r, const std::string& hint, bool enabled = true);
    // 取消全部输入框焦点（点击按钮/输入框之外的空白处时调用）
    void clear_text_focus();
    void text(const std::string& s, float x, float y, float size, Color c);
    float text_width(const std::string& s, float size);
    void load_ui_font();
    void reload_font();
    void add_codepoints(const std::string& s);
    // 文本里出现尚未加载的字符时登记（玩家名字可以是任意汉字）。只收集不立即重建：
    // 一条 room_state / game_start 会带来整队甚至全场的名字，逐个立即重建等于
    // 同一帧里把整张字体图集反复重做；新字形由 flush_glyph_reload 每帧统一重建一次
    void ensure_glyphs(const std::string& s);
    void flush_glyph_reload();
    // 设置状态栏消息。服务器/采样错误是运行时才见的动态字符串，经此登记字形，
    // 否则没在 kUiTexts 里出现过的汉字会显示成占位符
    void set_status(const std::string& s);

    // 单机（本地 Room 产出与服务器同格式的消息，交给 handle_server_message 统一处理）
    void start_local_game();
    void tick_local();

    // 联机
    void connect_server();
    void process_network();
    void handle_server_message(const json& m);
    // 房主改"下一把"的配置：把改动并入下一把配置（match_cfg_）后发 set_config
    void send_room_config(const json& patch);
    // 房主判定要带 id 有效性：member_id/host 缺省都是 -1，只比相等会把"未知"误判成房主
    bool is_host() const {
        return in_room_ && my_member_id_ >= 0 && my_member_id_ == host_member_id_;
    }
    // 我是否在观战：观战者不在对局阵容里（联机中途进入观战或单机"只看人机互打"）
    bool am_spectating() const;
    // 帮助面板是战斗界面的模态，只有它真的画在 Battle 屏上时才屏蔽输入：断线/被踢
    // 带着"开着帮助"的状态回大厅时，若照样全局屏蔽，大厅按钮会全部失灵且无面板可关
    bool help_modal_active() const;

    // 提交与预览
    CurveRequest build_request_from_ui() const;
    bool can_submit() const;
    void submit_current();
    void update_preview();
    Vec2 my_position() const;
    // 我的视角是否镜像：参战看队伍（B 队在 mirror_view 房间）；观战者用本地切换键
    bool am_i_mirrored() const;
    void toggle_spectate_view() { spectate_mirror_ = !spectate_mirror_; }

    // 状态
    Screen screen_ = Screen::Menu;
    bool quit_ = false;  // "退出游戏"请求：置位后主循环自然退出，让析构完成清理
    std::map<int, Font> fonts_;       // 档位字号 → 字形图集（kFontTiers；见 ui_style.h）
    std::vector<int> glyph_cps_;      // 当前字体已包含的码点（升序去重）
    bool glyph_dirty_ = false;        // 收集到新字形、待帧末统一重建图集
    bool font_is_default_ = false;    // 所有候选中文字体都加载失败
    float frame_dt_ = 1.0f / 60.0f;
    bool ui_click_consumed_ = false;  // 本帧的鼠标点击是否落在按钮/输入框上（绘制期间收集）
    bool help_open_ = false;          // 对战界面的函数帮助面板是否打开（打开时屏蔽按钮与输入）
    std::string status_;
    std::string result_text_;

    // 单机
    std::unique_ptr<Room> local_;
    // AI 思考线程池：单机人机的决策在池里算，渲染帧不被规划计算卡住
    ThinkPool think_pool_{2};
    bool online_ = false;

    // 联机
    NetClient net_;
    std::string server_host_ = "127.0.0.1";
    int server_port_ = 9527;
    std::string player_name_ = "玩家";
    int my_member_id_ = -1;
    int my_room_id_ = -1;
    int host_member_id_ = -1;
    bool in_room_ = false;
    bool create_cancelled_ = false;  // 创建房间界面点了"返回"：迟到的 joined 到达时代为退出
    bool silent_leave_ = false;      // 代发退出中：left_room 到达时不弹"已离开房间"提示
    json room_members_ = json::array();
    json room_list_ = json::array();
    // 配置回显拆两份（对应 room_state 的 room/config 两个对象）：
    json room_cfg_ = json::object();   // 房间的属性（士兵数/坐标系，界面上只读展示）
    json match_cfg_ = json::object();  // "下一把"配置（next_match 回显，规则面板据此高亮）
    // 房间的种子策略：next_match.seed 非 null = 固定种子（种子按钮高亮与文案的判据）
    bool room_seed_fixed() const {
        if (!match_cfg_.is_object()) return false;
        const auto it = match_cfg_.find("seed");
        return it != match_cfg_.end() && !it->is_null();
    }
    TextBox room_seed_box_;            // 房主指定固定种子时的输入框
    int member_first_ = 0;             // 房间成员列表首个可见行（满员装不下时滚轮滚动）

    // 创建房间的参数
    TextBox create_seed_box_;
    TextBox create_password_box_;  // 房间密码（明文输入，发送前本地 SHA-256；不做掩码）
    TextBox join_password_box_;    // 大厅：加入有密码的房间时填（同上）
    int create_obstacles_ = 15;
    int create_first_team_ = -1;    // -1 随机 / 0 A 队 / 1 B 队
    int create_soldiers_ = 3;       // 每人士兵数（1~4，创建后不可更改）
    int create_coord_field_ = 0;    // 曲线参考系（0=以你为原点 1=地图坐标，创建后不可更改）

    // 单机设置
    int set_team_size_ = 1;
    int set_soldiers_ = 3;    // 每人士兵数（1~4，全场统一）
    int set_difficulty_ = 1;  // 0 简单 1 普通 2 困难 3 大神
    bool local_spectate_ = false;  // 单机：以观战身份开局（不进对局，只看人机互打）
    int set_hp_ = 1;
    int set_obstacles_ = 15;
    int set_first_team_ = -1;      // 先手：-1 随机 / 0 A 队 / 1 B 队
    int set_input_seconds_ = 120;  // 单机：每回合输入时限（秒）
    int set_coord_field_ = 0;      // 单机：曲线参考系 0=以士兵为原点 / 1=地图坐标（同房间属性）
    int ai_diff_ = 1;              // 大厅里"添加人机"选用的难度

    // update_preview 的输入缓存：输入完全没变就沿用上次预览（ExprTk 编译与曲线采样不必每帧重做）
    struct PreviewKey {
        std::string a, b;
        int mode = -1;
        int dir = -1;
        std::optional<double> ps, pe;  // 与 CurveRequest 字段同类型：未填参数时比较语义才正确
        bool mirror = false;           // 观战者切换视角也要重算预览
        std::string angle;             // 发射角输入框原文（Ypp 用）
        bool used = false;
    } preview_key_;

    // 输入
    TextBox expr_a_;
    TextBox expr_b_;
    TextBox p_start_;
    TextBox p_end_;
    TextBox angle_box_;  // Ypp 的发射角（单位：度，-85~85；内容沿袭不随回合清空）
    TextBox host_box_;
    TextBox name_box_;
    TextBox seed_box_;
    int mode_index_ = 0;  // 0..6，对应 kModes
    int dir_index_ = 0;   // 0 = 正方向，1 = 负方向
    // 观战者的本地镜像视角开关（房间开了 mirror_view 时才能切换显示）
    bool spectate_mirror_ = false;
    // 实时预览广播的节流：上次发出的文本与时刻（GetTime 秒）
    std::string sent_preview_text_;
    double last_preview_sent_ = -1e9;

    // 对战视图（本地与联机共用）
    struct Battle {
        GameConfig cfg;
        const std::vector<Player>* players = nullptr;
        const std::vector<Obstacle>* obstacles = nullptr;
        std::vector<Player> online_players;    // 联机时的数据存储
        std::vector<Obstacle> online_obstacles;
        int current_player = -1;
        double input_left = 0.0;
        double turn_start_left = 0.0;  // 本回合开始时服务器给的时限（掉帧后按时间差重算显示的基准）
        double turn_begin_time = 0.0;  // 收到 turn_begin 的本地时刻（GetTime 秒）
        int winner = -1;
        bool active = false;    // 对局进行中
        bool submitted = false; // 本回合我已提交（飞行阶段等待结束）
        bool leaving_spectate = false;  // 观战者已点"退出观战"、等服务器回应（防连点）
        bool restart_sent = false;      // 已点"再来一局"、等开局或错误回应（防连点）
        bool has_projectile = false;
        Vec2 projectile;
        double projectile_travel = 0.0;      // 本地弹头弧长（每帧向外推目标平滑收敛）
        double projectile_speed = 0.0;       // 炮弹速度（shot 消息带回；本地推进用）
        double projectile_net_travel = 0.0;  // 服务器广播来的最新 travel（外推基准）
        double projectile_msg_time = 0.0;    // 最近一次收到广播的本地时刻（GetTime 秒）
        bool projectile_stopped = false;     // 服务器 travel 不再增长=炮弹已停（销毁）
        Path projectile_path;
        bool has_path = false;
        Path preview_path;
        bool has_preview = false;
        int my_id = -1;
        int round = 0;
        // 下一个出手的玩家/士兵（服务器随 turn_begin 广播，地图上给小标记）
        int next_player = -1;
        int next_soldier = 0;
        // 实时函数预览：当前回合玩家正在写的内容（服务器只转发当前回合玩家的）
        std::string preview_text;
        // 各玩家"最近用过的函数"（从 shot 消息累积，右侧面板行内小字展示）
        std::map<int, std::string> last_func;
        // 跳过当前地图的投票：已投的成员 id 与参战人类总数（total<=0 = 没有投票显示）
        std::vector<int> skip_voted;
        int skip_total = 0;
    } battle_;
};

}  // namespace fg
