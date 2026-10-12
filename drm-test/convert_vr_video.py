import subprocess, struct, os, glob

os.makedirs('/tmp/frames_vr', exist_ok=True)
for f in glob.glob('/tmp/frames_vr/*.jpg'): os.remove(f)

# Use shortest=1 or specify input duration so color background doesn't loop forever!
filter_complex = (
    "[0:v]scale=960:540[vid];"
    "color=c=black:s=3664x1920:d=34.4[bg];"
    "[bg][vid]overlay=436:690:shortest=1[tmp1];"
    "[tmp1][vid]overlay=2268:690:shortest=1[canvas];"
    "[canvas]transpose=1[out]"
)

cmd = [
    '/opt/homebrew/bin/ffmpeg', '-y',
    '-i', '/Users/waffles/Downloads/testvideo.mp4',
    '-filter_complex', filter_complex,
    '-map', '[out]',
    '-q:v', '5',
    '/tmp/frames_vr/frame_%04d.jpg'
]

print("Running ffmpeg stereo VR compositor...")
subprocess.check_call(cmd)

frame_files = sorted(glob.glob('/tmp/frames_vr/frame_*.jpg'))
print(f"Extracted {len(frame_files)} stereo frames (1920x3664)")

out_file = '/tmp/testvideo_vr.qkxv'
with open(out_file, 'wb') as out:
    out.write(struct.pack('<4sIIII', b'QKXV', len(frame_files), 30, 1920, 3664))
    sizes = [os.path.getsize(f) for f in frame_files]
    out.write(struct.pack(f'<{len(sizes)}I', *sizes))
    for f in frame_files:
        with open(f, 'rb') as ff:
            out.write(ff.read())

print(f"Created {out_file}: {os.path.getsize(out_file) / (1024*1024):.2f} MB")
