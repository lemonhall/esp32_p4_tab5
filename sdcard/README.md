# SD Card Files

把此目录下的文件拷贝到 SD 卡根目录，用于 Tab5 运行时加载资源。

## `font.ttf`
- 用途：IRC 消息中文显示（固件侧从 `/sd/font.ttf` 加载）
- 字体：LXGW WenKai（霞鹜文楷）Regular
- 许可证：SIL Open Font License 1.1（见 `sdcard/OFL.txt`）

### 安装到 SD（Windows 示例）
假设 SD 盘符为 `G:`：
```powershell
Copy-Item -Force .\sdcard\font.ttf G:\font.ttf
```

