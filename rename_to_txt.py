import os
import sys
import shutil

# ============================================
# Skripta za pripravo Park_auto_last/ mape
# 1. Zbrise Park_auto_last/ ce obstaja
# 2. Kopira src/ v Park_auto_last/
# 3. Kopira platformio.ini v Park_auto_last/
# 4. Kopira include/ v Park_auto_last/include/
# 5. Vsem datotekam doda .txt koncnico
# ============================================

SRC_DIR = 'src'
DST_DIR = 'Park_auto_last'

# 1. Zbrisi Park_auto_last ce obstaja
if os.path.isdir(DST_DIR):
    print('Brisem ' + DST_DIR + '/ ...')
    shutil.rmtree(DST_DIR)

# 2. Kopiraj src/ v Park_auto_last/
print('Kopiram ' + SRC_DIR + '/ -> ' + DST_DIR + '/ ...')
shutil.copytree(SRC_DIR, DST_DIR)

# 3. Kopiraj platformio.ini v Park_auto_last/
if os.path.isfile('platformio.ini'):
    shutil.copy2('platformio.ini', os.path.join(DST_DIR, 'platformio.ini'))
    print('  kopiram platformio.ini')

# 4. Kopiraj include/ v Park_auto_last/include/
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
