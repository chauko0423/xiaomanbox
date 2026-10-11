# XiaoManBox Basic

从原工程独立裁剪的基础版：本地对话、会话、文件与命令工具、模型配置、技能、MCP、UE/蓝图与本地画板。账号与团队交流已删除。默认用户数据目录 ~/.xiaomanbox，可用 XIAOMAN_HOME 覆盖。

建议在上一级部署包运行 `node deploy.mjs install`，会同时构建 ../local-board。单独构建本目录：`npm ci`、`npm run type-check`、`npm run build`、`npm start`。打包：`npm run dist`，要求相邻 local-board 已安装依赖并完成 build。

详见上级人工部署说明和第三方归属。UE插件只有源码；量化权重和推理引擎没有塞进桌面安装包。根代码许可为待确认的Apache-2.0方案，插件仍用自身MIT许可。
