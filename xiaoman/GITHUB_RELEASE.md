# GitHub 量化权重分片发布与使用

只处理 release-manifest.json 中已有 SHA256 的量化权重。BF16、LoRA、训练数据与个人配置不会进入发布候选。首次准备默认256 MiB；已有分片清单时按清单尺寸重建。当前4片已验证的1 GiB大片继续复用，其余新增片最多256 MiB，均低于GitHub Release单文件2 GiB限制。

## 下载者

下载最新源码，先读人工部署说明。Release 分片实际上传并填写 URL 后：

```powershell
node deploy.mjs download --id xiaoman-v6-sd
```

视觉版使用 xiaoman-v6-sd-vis。清单 URL 为 null 表示尚未公开上传。手动下载全部分片后可加 `--parts-dir <下载目录>`；工具根据清单顺序合并并检查每片与完整文件 SHA256。损坏分片、缺片和完整文件不符都会报错，不替换已有模型。完成的分片支持中断后复用；单片下载中断需要重新下载该片。需要约两倍模型大小的可用空间。

## 发布者

先确保本地模型与清单一致，准备分片：

```powershell
node tools/prepare-model-release.mjs split --id xiaoman-v6-sd
node tools/prepare-model-release.mjs split --id xiaoman-v6-sd-vis
```

输出到 release-candidates/model-parts/，不会提交到普通 Git。使用 GitHub 官方 CLI 在网页完成设备授权：

```powershell
gh auth login --hostname github.com --git-protocol https --web
node tools/publish-model-release.mjs --gh gh
```

发布器固定核对 chauko0423 账号与 chauko0423/xiaomanbox 仓库。先建立预发布草稿，校验本地分片，再上传；已上传同名文件只有 GitHub 的大小和服务端 SHA256 均一致时才复用，不会覆盖不一致文件。所有分片与校验说明成功后才公开草稿并填写主清单 URL。最后提交更新的 release-manifest.json，并重做源码部署 ZIP。

上传失败后重新运行同一命令即可复用已确认的分片。登录凭据由 GitHub CLI 管理，不能放入部署 ZIP、源码、命令参数或聊天。模型来源与授权说明仍需维护；主线文本引擎已在独立Release公开，部署工具按模型绑定下载；视觉引擎同样已独立公开，按清单下载其绑定的4份LGPL共享库与许可说明。

官方文件大小限制：https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
