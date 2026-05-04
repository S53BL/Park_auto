import os
import sys

# Če podamo mapo kot argument, uporabi to mapo, sicer privzeto 'src_txt'
root = sys.argv[1] if len(sys.argv) > 1 else 'src_txt'

if not os.path.isdir(root):
    print(f'NAPAKA: Mapa "{root}" ne obstaja!')
    sys.exit(1)

count = 0
for dp, dn, fn in os.walk(root):
    for f in fn:
        src = os.path.join(dp, f)
        dst = os.path.join(dp, f + '.txt')
        os.rename(src, dst)
        count += 1
        print(f'{src} -> {dst}')

print(f'Preimenovanih {count} datotek v mapi "{root}" in podmapah.')
