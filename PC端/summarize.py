import json
import sys
import io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
with open(r'C:\Users\17402\Desktop\机器狗\PC端\batch_v3.json', encoding='utf-8') as f:
    lines = json.load(f)
print('idx  actions            reply     ms')
print('-' * 50)
for r in lines:
    print(f'{r["idx"]:<6} {r["actions"][:22]:<20} {r["reply"][:10]:<10} {r["ms"]:>5}')