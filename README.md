# 仓库作用
此仓库的作用主要是为了实现在 Android Platform 上实现低资源占用的实时音频降噪。

# 整体框架

此仓库包括完整 回声消除、语音增强、降噪。整体框架为 AAudio + WebRtc Audio Process Module + DeepFliterNet2 来处理输入音频。
其中WebRtc Apm 主要是进行回声消除、DeepFliterNet2 来进行降噪。

# 处理效果

<img width="1880" height="304" alt="e1c2d5cb9c4a03908e9709355863289f" src="https://github.com/user-attachments/assets/2f1ba52c-7b5f-4817-ab0e-64d6f35a5112" />
最上方为原始PCM波形图，下方为经过整体pipeline处理之后降噪波形图，对比得出结果，整体效果还不错。

# 测试环境
整体资源占用消耗过高，整体消耗达到了 80% 单核的占用率， 并且是否能够实时处理暂时没有验证过，后期主要将会对整体进行优化，思路偏向于将 WebRtc Apm的高频处理转移到 QSC6490 hexagon Dsp去计算。
对于神经网络这块，采用的是官方的libDF编译的so/a库，原始的 rust 代码中，推理 NN 部分 tract Runtime 完成，这个部分需要优化。

## 测试硬件
硬件为QCS6490.
