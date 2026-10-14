#pragma once
// 合成音效：全部由代码生成 PCM（无任何音频资源文件），风格对齐参考实现的
// WebAudio 振荡器音（发射扫频 / 命中短打 / 爆炸噪声 / 阵亡下坠 / 回合提示）。
// 无头与截图模式不初始化音频设备，所有 play 函数在设备未就绪时静默跳过。

namespace fg {

void sfx_init();      // 生成并加载全部音效（需窗口/音频设备已就绪；可重复调用）
void sfx_shutdown();  // 释放（进程退出前）
bool sfx_ready();

void sfx_shot();     // 发射：正弦扫频"咻"
void sfx_hit();      // 命中：短促方波"啪"
void sfx_death();    // 阵亡：低频下坠"咚"
void sfx_explode();  // 撞障碍/炸坑：噪声爆"轰"
void sfx_turn();     // 轮到我：轻提示音"叮"

}  // namespace fg
