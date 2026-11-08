import pypdfium2 as pdfium
from pathlib import Path
for file,pages in [('PY32F002A',[17,41]),('SY7200A',[1]),('Omron_FPC',[5,6]),('AP2112',[2])]:
 p=pdfium.PdfDocument('references/'+file+'.pdf')
 for n in pages:
  p[n].render(scale=1.4).to_pil().save('output/'+file+'_'+str(n+1)+'.png')
 print(file,len(p))
