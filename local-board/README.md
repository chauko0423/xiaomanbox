# 本地基础画板

React/Excalidraw + Yjs/Hocuspocus，无账户、无团队聊天。宿主在本机以随机管理钥匙创建画板；开启分享后通过编辑与只读链接供LAN协作。服务端执行只读约束、房间注册与图片鉴权，未知房间不能匿名创建。

在上级运行 `node deploy.mjs install`、`node deploy.mjs board`，管理入口由终端打印。本目录开发构建：`npm ci`、`npm run build`。`node server/index.mjs`启动服务；XIAOMAN_BOARD_PORT默认8880，XIAOMAN_BOARD_DATA指定数据目录。更换域名需由部署者自行配置TLS/反向代理；前端同源HTTP/WS。

内容在宿主磁盘；链接含访问密钥。首次状态无分享，只允许回环访问。关闭分享会断开已有连接，之后LAN重新连接受拒。换钥匙使旧编辑/只读链接失效。导出文件供备份；未提供版本历史、账号找回与团队知识库。自动验收见 ../tests/acceptance.mjs。
