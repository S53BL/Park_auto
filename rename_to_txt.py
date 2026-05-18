import os
import sys
import shutil

# ============================================
# Skripta za pripravo Park_auto_last/ mape
# 1. Zbrise Park_auto_last/ ce obstaja
# 2. Kopira src/ v Park_auto_last/src/
# 3. Kopira platformio.ini v Park_auto_last/
# 4. Kopira partitions_custom.csv v Park_auto_last/
# 5. Kopira CHANGELOG.md v Park_auto_last/
# 6. Kopira decisions.md v Park_auto_last/
# 7. Kopira to_do.md v Park_auto_last/
# 8. Kopira include/ v Park_auto_last/include/
# 9. Kopira data/ v Park_auto_last/data/
# 10. Kopira scripts/ v Park_auto_last/scripts/
# 11. Vsem datotekam doda .txt koncnico
# ============================================

SRC_DIR = 'src'
DST_DIR = 'Park_auto_last'

# 1. Zbrisi Park_auto_last ce obstaja
if os.path.isdir(DST_DIR):
    print('Brisem ' + DST_DIR + '/ ...')
    shutil.rmtree(DST_DIR)

# 2. Kopiraj src/ v Park_auto_last/src/
dst_src = os.path.join(DST_DIR, SRC_DIR)
print('Kopiram ' + SRC_DIR + '/ -> ' + dst_src + '/ ...')
shutil.copytree(SRC_DIR, dst_src)

# 3. Kopiraj platformio.ini v Park_auto_last/
if os.path.isfile('platformio.ini'):
    shutil.copy2('platformio.ini', os.path.join(DST_DIR, 'platformio.ini'))
    print('  kopiram platformio.ini')

# 4. Kopiraj partitions_custom.csv v Park_auto_last/
if os.path.isfile('partitions_custom.csv'):
    shutil.copy2('partitions_custom.csv', os.path.join(DST_DIR, 'partitions_custom.csv'))
    print('  kopiram partitions_custom.csv')

# 5. Kopiraj CHANGELOG.md v Park_auto_last/
if os.path.isfile('CHANGELOG.md'):
    shutil.copy2('CHANGELOG.md', os.path.join(DST_DIR, 'CHANGELOG.md'))
    print('  kopiram CHANGELOG.md')

# 6. Kopiraj decisions.md v Park_auto_last/
if os.path.isfile('decisions.md'):
    shutil.copy2('decisions.md', os.path.join(DST_DIR, 'decisions.md'))
    print('  kopiram decisions.md')

# 7. Kopiraj to_do.md v Park_auto_last/
if os.path.isfile('to_do.md'):
    shutil.copy2('to_do.md', os.path.join(DST_DIR, 'to_do.md'))
    print('  kopiram to_do.md')

# 8. Kopiraj include/ v Park_auto_last/include/
if os.path.isdir('include'):
    dst_include = os.path.join(DST_DIR, 'include')
    shutil.copytree('include', dst_include)
    print('  kopiram include/')

# 9. Kopiraj data/ v Park_auto_last/data/
if os.path.isdir('data'):
    dst_data = os.path.join(DST_DIR, 'data')
    shutil.copytree('data', dst_data)
    print('  kopiram data/')

# 10. Kopiraj scripts/ v Park_auto_last/scripts/
if os.path.isdir('scripts'):
    dst_scripts = os.path.join(DST_DIR, 'scripts')
    shutil.copytree('scripts', dst_scripts)
    print('  kopiram scripts/')

# 11. Dodaj .txt vsem datotekam
count = 0
for dp, dn, fn in os.walk(DST_DIR):
    for f in fn:
        src = os.path.join(dp, f)
        dst = os.path.join(dp, f + '.txt')
        os.rename(src, dst)
        count += 1
        print('  ' + src + ' -> ' + dst)

print('Koncano! Preimenovanih ' + str(count) + ' datotek.')
