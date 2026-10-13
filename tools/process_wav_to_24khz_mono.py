import os
import wave
import struct
import numpy as np
from scipy.signal import resample_poly
from math import gcd

def read_wav_with_smpl(file_path):
    """
    Читает WAV-файл и дополнительно ищет в нем чанк 'smpl' для извлечения точек цикла.
    """
    with open(file_path, 'rb') as f:
        riff_header = f.read(12)
        if riff_header[:4] != b'RIFF' or riff_header[8:12] != b'WAVE':
            raise ValueError("Некорректный WAV файл")
            
        fmt_params = None
        data_bytes = None
        loop_data = None
        
        while True:
            chunk_header = f.read(8)
            if len(chunk_header) < 8:
                break
            chunk_id = chunk_header[:4]
            chunk_size = struct.unpack('<I', chunk_header[4:])[0]
            chunk_data = f.read(chunk_size)
            
            # Четное выравнивание по байтам в RIFF
            if chunk_size % 2 != 0:
                f.read(1)
                
            if chunk_id == b'fmt ':
                nchannels, sampwidth, framerate, _, _, _ = struct.unpack('<HHIIHH', chunk_data[:16])
                fmt_params = (nchannels, sampwidth, framerate)
            elif chunk_id == b'data':
                data_bytes = chunk_data
            elif chunk_id == b'smpl':
                if len(chunk_data) >= 36:
                    num_loops = struct.unpack('<I', chunk_data[28:32])[0]
                    if num_loops > 0:
                        loop_entry = chunk_data[36:36+24]
                        if len(loop_entry) >= 24:
                            _, loop_type, start, end, _, _ = struct.unpack('<IIIIII', loop_entry)
                            loop_data = {'type': loop_type, 'start': start, 'end': end}
                            
        return fmt_params, data_bytes, loop_data

def write_wav_with_smpl(output_path, channels, sampwidth, framerate, pcm_data, loop_data=None):
    """
    Записывает WAV-файл с аудиоданными и добавляет 'smpl' чанк, если он есть.
    """
    nframes = len(pcm_data)
    audio_bytes = pcm_data.tobytes()
    data_size = len(audio_bytes)
    
    fmt_chunk = b'fmt ' + struct.pack('<IHHIIHH', 16, 1, channels, framerate, framerate * channels * sampwidth, channels * sampwidth, sampwidth * 8)
    data_chunk = b'data' + struct.pack('<I', data_size) + audio_bytes
    
    chunks = [fmt_chunk, data_chunk]
    
    if loop_data is not None:
        manufacturer = 0
        product = 0
        sample_period = int(1000000000 / framerate)
        midi_unity_note = 60
        midi_pitch_fraction = 0
        smpte_format = 0
        smpte_offset = 0
        num_loops = 1
        sampler_data = 0
        
        cue_point_id = 0
        loop_type = loop_data['type']
        start_frame = int(loop_data['start'])
        end_frame = int(loop_data['end'])
        fraction = 0
        play_count = 0
        
        # Исправлено: ровно 15 элементов (15 букв 'I')
        smpl_payload = struct.pack(
            '<IIIIIIIIIIIIIII',
            manufacturer, product, sample_period, midi_unity_note,
            midi_pitch_fraction, smpte_format, smpte_offset, num_loops,
            sampler_data,
            cue_point_id, loop_type, start_frame, end_frame, fraction, play_count
        )
        smpl_chunk = b'smpl' + struct.pack('<I', len(smpl_payload)) + smpl_payload
        chunks.insert(1, smpl_chunk)
        
    riff_size = 4 + sum(len(c) for c in chunks)
    with open(output_path, 'wb') as f:
        f.write(b'RIFF')
        f.write(struct.pack('<I', riff_size))
        f.write(b'WAVE')
        for c in chunks:
            f.write(c)

def process_wavs_with_loops(input_folder, output_folder, target_framerate=24000):
    os.makedirs(output_folder, exist_ok=True)
    files = [f for f in os.listdir(input_folder) if f.lower().endswith('.wav')]
    print(f"Найдено WAV-файлов: {len(files)}")
    
    success_count = 0
    for filename in files:
        input_path = os.path.join(input_folder, filename)
        output_path = os.path.join(output_folder, filename)
        
        try:
            fmt_params, data_bytes, loop_data = read_wav_with_smpl(input_path)
            if fmt_params is None or data_bytes is None:
                print(f"[Пропуск] {filename} — некорректная структура WAV.")
                continue
                
            nchannels, sampwidth, framerate = fmt_params
            if sampwidth != 2:
                print(f"[Пропуск] {filename} — поддерживается только 16-бит PCM.")
                continue
                
            data = np.frombuffer(data_bytes, dtype=np.int16)
            
            if nchannels == 2:
                left = data[0::2].astype(np.float32)
                right = data[1::2].astype(np.float32)
                mono_data = (left + right) / 2.0
            else:
                mono_data = data.astype(np.float32)
                
            ratio = target_framerate / framerate
            if framerate == target_framerate:
                resampled_data = mono_data
            else:
                g = gcd(target_framerate, framerate)
                up = target_framerate // g
                down = framerate // g
                resampled_data = resample_poly(mono_data, up, down)
                
            resampled_data = np.clip(resampled_data, -32768, 32767).astype(np.int16)
            
            new_loop_data = None
            if loop_data is not None:
                new_start = int(round(loop_data['start'] * ratio))
                new_end = int(round(loop_data['end'] * ratio))
                new_start = min(new_start, len(resampled_data) - 1)
                new_end = min(new_end, len(resampled_data) - 1)
                
                new_loop_data = {
                    'type': loop_data['type'],
                    'start': new_start,
                    'end': new_end
                }
                print(f"  [{filename}] Петля: start={loop_data['start']}->{new_start}, end={loop_data['end']}->{new_end}")
            
            write_wav_with_smpl(output_path, 1, sampwidth, target_framerate, resampled_data, new_loop_data)
            success_count += 1
            
        except Exception as e:
            print(f"[Ошибка] Не удалось обработать {filename}: {e}")
            
    print(f"\nГотово! Успешно обработано файлов: {success_count} из {len(files)}")

if __name__ == "__main__":
    INPUT_DIR = "stereo_samples"
    OUTPUT_DIR = "mono_loops_24khz_samples"
    
    process_wavs_with_loops(INPUT_DIR, OUTPUT_DIR, target_framerate=24000)