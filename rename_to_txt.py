import os
import sys
import shutil

# ============================================
# Skripta za pripravo src_txt/ mape
# 1. Zbrise src_txt/ ce obstaja
# 2. Kopira src/ v src_txt/
# 3. Kopira platformio.ini v src_txt/
# 4. Kopira include/ v src_txt/include/
# 5. Vsem datotekam doda .txt koncnico
# ============================================

SRC_DIR = 'src'
DST_DIR = 'src_txt'

# 1. Zbrisi src_txt ce obstaja
if os.path.isdir(DST_DIR):
    print('Brisem ' + DST_DIR + '/ ...')
    shutil.rmtree(DST_DIR)

# 2. Kopiraj src/ v src_txt/
print('Kopiram ' + SRC_DIR + '/ -> ' + DST_DIR + '/ ...')
shutil.copytree(SRC_DIR, DST_DIR)

# 3. Kopiraj platformio.ini v src_txt/
if os.path.isfile('platformio.ini'):
    shutil.copy2('platformio.ini', os.path.join(DST_DIR, 'platformio.ini'))
    print('  kopiram platformio.ini')

# 4. Kopiraj include/ v src_txt/include/
if os.path.isdir('include'):
    dst_include = os.path.join(DST_DIR, 'include')
    shutil.copytree('include', dst_include)
    print('  kopiram include/')

# 5. Dodaj .txt vsem datotekam
count = 0
for dp, dn, fn in os.walk(DST_DIR):
    for f in fn:
        src = os.path.join(dp, f)
        dst = os.path.join(dp, f + '.txt')
        os.rename(src, dst)
        count += 1
        print('  ' + src + ' -> ' + dst)

print('Koncano! Preimenovanih ' + str(count) + ' datotek.')
