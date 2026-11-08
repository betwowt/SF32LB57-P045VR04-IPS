from pathlib import Path
import shutil
import pypdfium2 as pdfium
from PIL import Image, ImageChops
root=Path.cwd()
for d in ['tmp/pdfs','references/user_images','references/rendered']:(root/d).mkdir(parents=True,exist_ok=True)
images=['6f230122-2819-4a91-a970-75ce9250cab3','b3d01c52-ab46-4035-a9e9-a2249e9bd609','510d637f-8737-4eea-b973-c7bc44d7d40f','1573e20a-cb15-4099-a119-5603810b8e96','4a0879bc-e55d-46c8-be08-5663f45a9326','d275922a-384c-4187-9007-c6a0e37e363e','52170e38-eaa0-4b8d-826e-fa061c4b22ee']
for i,n in enumerate(images,1):shutil.copy2(Path('C:/Users/betwowt/AppData/Local/Temp')/('codex-clipboard-'+n+'.png'),root/'references/user_images'/f'image_{i:02}.png')
# Render native KiCad layout PDF, not a substitute visual design.
doc=pdfium.PdfDocument(str(root/'tmp/pdfs/layout_review.pdf'))
im=doc[0].render(scale=2).to_pil().convert('RGB')
bounds=ImageChops.difference(im,Image.new('RGB',im.size,'white')).getbbox()
if bounds:
 x1,y1,x2,y2=bounds;im=im.crop((max(0,x1-20),max(0,y1-20),min(im.width,x2+20),min(im.height,y2+20)))
im.save(root/'output/pcb_layout.png')
print('Saved source screenshots and final layout preview.')
