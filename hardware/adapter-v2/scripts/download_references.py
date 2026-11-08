from pathlib import Path
import urllib.request
items={
'PY32F002A.pdf':'https://www.puyasemi.com/download_path/%E6%95%B0%E6%8D%AE%E6%89%8B%E5%86%8C/MCU%20%E5%BE%AE%E5%A4%84%E7%90%86%E5%99%A8/PY32F002A_Datasheet_V0.2.pdf',
'SY7200A.pdf':'https://www.silergy.com/download/downloadFile?ftype=note&id=4351&type=product',
'Omron_FPC.pdf':'https://components.omron.com/system/files/2025-03/datasheet_pdf/G146-E1.pdf',
'AP2112.pdf':'https://www.diodes.com/datasheet/download/AP2112.pdf'}
for n,u in items.items():
 try:
  request=urllib.request.Request(u,headers={'User-Agent':'Mozilla/5.0'})
  with urllib.request.urlopen(request,timeout=30) as resp: data=resp.read()
  if not data.startswith(b'%PDF'): raise ValueError('Not PDF')
  Path('references',n).write_bytes(data)
  print(n,len(data))
 except Exception as e: print(n,str(e))
