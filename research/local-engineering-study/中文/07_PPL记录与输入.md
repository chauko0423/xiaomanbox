# PPL记录与输入重建

六份原报告以机器路径去标识化的数字派生版发布于`data/ppl`。47811个评分token、47812个流token及512/4096/24576上下文设置不变。离线程序重算每窗NLL总和及exp(total_nll/scored_tokens)。没有新增模型评分。旧/新比较支持部署分析，不能证明任务完成改善，也不是完整标准语料基准成绩。

事后来源核查将本地输入逐字节匹配到[Salesforce WikiText-2 raw-v1](https://huggingface.co/datasets/Salesforce/wikitext/tree/b08601e04326c79dfdd32d625aee71d232d685c3/wikitext-2-raw-v1)的前缀，固定提交`b08601e04326c79dfdd32d625aee71d232d685c3`。Parquet SHA256为`5f1bea067869d04849c0f975a2b29c4ff47d867f484f5010ea5e861eab246d91`。保留测试集全部行与顺序，以一个LF连接，将LF规范化为CRLF、UTF-8编码，取前201377字节；其SHA256为`bf5b371fec7f2dcccdee81b8e0f81f0565c09421e7291a2f27fb32b9d0cfd6d6`。完整重建测试文本也匹配单独记录的本地哈希。非raw-v1候选未匹配，其失败在套件中保留。

```powershell
python -m pip install pyarrow==20.0.0
python -B reconstruct_ppl_corpus.py --output C:/study/long_48k.txt
```

公开重建程序已获取固定公开Parquet、核验完整文本及输入哈希，在新本地路径生成精确输入，模型请求为零。这证明输入字节可以重建，不能补成原实验前的来源时间戳。token数量来自原模型/分词器记录，不按字节长度推算。原数据集及许可说明由上游保留，此处分发报告与重建配方，不分发原数据集文本、不授予新语料许可。重新进行PPL评分仍需另行提供精确产物与评分可执行文件或源码构建。原报告绑定模型名称及签名，不绑定新重建的引擎版本。
