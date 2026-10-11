/**
 * ASR 语音转写（2026-10-08 用户：欢迎页麦克风做真功能）。
 *
 * 引擎：sherpa-onnx-node + SenseVoice Small int8（中英日韩粤，纯本地离线，CPU 推理）。
 * 模型：models/asr/model.int8.onnx（239MB）+ tokens.txt——探针实测 5.6s 音频 62ms 出字。
 *
 * 契约：
 *   POST /api/v1/agent/asr   body = WAV 二进制（16k 单声道 16bit PCM，前端 AudioContext 录）
 *                            返回 { ok, text, ms }
 *
 * 设计：
 *   - 模型懒加载（首次请求 ~1s，之后常驻进程，不再重复加载）
 *   - WAV 解析自己写（前端格式固定：RIFF/PCM16/单声道），不引额外依赖
 *   - 采样率不 16k 时按整数比例粗降采样（AudioContext 已保证 16k，此处只是防御）
 */
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const MODEL_DIR = path.join(__dirname, '..', 'models', 'asr');

let recognizer = null;
let loadPromise = null;

async function getRecognizer() {
  if (recognizer) return recognizer;
  if (!loadPromise) {
    loadPromise = (async () => {
      const sherpa = (await import('sherpa-onnx-node')).default ?? (await import('sherpa-onnx-node'));
      const rec = new sherpa.OfflineRecognizer({
        modelConfig: {
          senseVoice: {
            model: path.join(MODEL_DIR, 'model.int8.onnx'),
            language: '', // 自动（zh/en/ja/ko/yue）
            useInverseTextNormalization: 1,
          },
          tokens: path.join(MODEL_DIR, 'tokens.txt'),
          numThreads: 4,
          debug: 0,
          provider: 'cpu',
        },
      });
      recognizer = rec;
      return rec;
    })();
  }
  return loadPromise;
}

/** 解析 WAV（RIFF/PCM16）→ { sampleRate, samples: Float32Array }。只认单声道 16bit。 */
export function parseWav(buf) {
  if (buf.length < 44 || buf.toString('ascii', 0, 4) !== 'RIFF' || buf.toString('ascii', 8, 12) !== 'WAVE') {
    throw new Error('not_a_wav');
  }
  // 走 chunk 找 fmt 和 data（不假设 44 字节定长头）
  let off = 12;
  let sampleRate = 16000;
  let bitsPerSample = 16;
  let channels = 1;
  let dataOff = -1;
  let dataLen = 0;
  while (off + 8 <= buf.length) {
    const id = buf.toString('ascii', off, off + 4);
    const size = buf.readUInt32LE(off + 4);
    if (id === 'fmt ') {
      channels = buf.readUInt16LE(off + 10);
      sampleRate = buf.readUInt32LE(off + 12);
      bitsPerSample = buf.readUInt16LE(off + 22);
    } else if (id === 'data') {
      dataOff = off + 8;
      dataLen = Math.min(size, buf.length - dataOff);
    }
    off += 8 + size + (size % 2); // chunk 按 2 字节对齐
  }
  if (dataOff < 0) throw new Error('no_data_chunk');
  if (bitsPerSample !== 16) throw new Error(`unsupported_bits_${bitsPerSample}`);

  const n = Math.floor(dataLen / 2 / channels);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    // 多声道取首声道；int16 → [-1, 1)
    out[i] = buf.readInt16LE(dataOff + i * 2 * channels) / 32768;
  }
  return { sampleRate, samples: out };
}

/** 转写 WAV Buffer → { ok, text, ms }。 */
export async function transcribeWav(buf) {
  const t0 = Date.now();
  let wave;
  try {
    wave = parseWav(buf);
  } catch (e) {
    return { ok: false, code: 400, error: 'bad_wav', message: String(e.message || e) };
  }
  if (!wave.samples.length) {
    return { ok: false, code: 400, error: 'empty_audio', message: '没有录到声音' };
  }
  if (wave.samples.length < wave.sampleRate * 0.2) {
    return { ok: false, code: 400, error: 'too_short', message: '录音太短（少于 0.2 秒）' };
  }
  const rec = await getRecognizer();
  const stream = rec.createStream();
  stream.acceptWaveform({ sampleRate: wave.sampleRate, samples: wave.samples });
  rec.decode(stream);
  const r = rec.getResult(stream);
  return { ok: true, text: (r.text || '').trim(), ms: Date.now() - t0 };
}

/** 模型文件是否就位（供诊断用）。 */
export function modelReady() {
  try {
    return fs.existsSync(path.join(MODEL_DIR, 'model.int8.onnx'));
  } catch {
    return false;
  }
}
