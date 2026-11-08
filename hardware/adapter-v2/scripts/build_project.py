"""Generate the editable KiCad schematic and its auditable connectivity manifest.

Run with Python 3.11+. The PCB is generated separately with KiCad's Python.
Do not run after manual edits unless regenerating is intended.
"""
from pathlib import Path
import json, uuid, csv, re, math, shutil

ROOT = Path(__file__).resolve().parents[1]
NAME = 'H743_RGB_Adapter'
LIB = ROOT / 'Adapter.kicad_sym'
KICAD = Path(r'C:\Program Files\KiCad\10.0\share\kicad')
def uid(s): return str(uuid.uuid5(uuid.NAMESPACE_URL, 'h743-rgb-adapter/'+s))
def q(s): return json.dumps(str(s), ensure_ascii=False)
def f(x): return f'{x:.4f}'.rstrip('0').rstrip('.') if x else '0'
def effects(size=1.27, extra=''): return f'(effects (font (size {size} {size})) {extra})'
def prop(key,val,x=0,y=0,hide=False,size=1.27):
    return f'(property {q(key)} {q(val)} (at {f(x)} {f(y)} 0) {effects(size,"(hide yes)" if hide else "")})'
def rect(x1,y1,x2,y2):
    return f'(rectangle (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.254) (type default)) (fill (type background)))'
def line(points,width=.254):
    return f'(polyline (pts {" ".join(f"(xy {f(x)} {f(y)})" for x,y in points)}) (stroke (width {width}) (type default)) (fill (type none)))'

symbols={}
def make_symbol(name,pins,graphics,ref='U',halfheight=10.16,desc='',hide_names=False):
    # pin: number, name, x, y, angle, electrical type, length
    head=f'(symbol {q(name)} (pin_names (offset 0.635) {"(hide yes)" if hide_names else ""}) (in_bom yes) (on_board yes)'
    head+=prop('Reference',ref,0,halfheight+5.08)+prop('Value',name,0,halfheight+2.54)
    head+=prop('Footprint','',hide=True)+prop('Datasheet','',hide=True)+prop('Description',desc,hide=True)
    head+=f'(symbol {q(name+"_0_1")} {graphics})'
    head+=f'(symbol {q(name+"_1_1")} '
    for no,nm,x,y,a,t,length in pins:
        head+=f'(pin {t} line (at {f(x)} {f(y)} {a}) (length {length}) (name {q(nm)} {effects(1.016)}) (number {q(no)} {effects(1.016)}))'
    head+='))'
    symbols[name]={'text':head,'pins':{str(p[0]):p for p in pins},'h':halfheight}

def box_symbol(name,left,right,width=15.24,ref='U',desc=''):
    h=(max(len(left),len(right))+1)*2.54/2
    # enforce 1.27 mm grid, pin spacing 2.54 mm
    pins=[]
    for side,items in [(-1,left),(1,right)]:
        for i,(no,nm,t) in enumerate(items):
            pins.append((str(no),nm,side*(width+5.08),h-2.54-i*2.54,0 if side<0 else 180,t,5.08))
    make_symbol(name,pins,rect(-width,h,width,-h),ref,h,desc)

make_symbol('R', [('1','',-5.08,0,0,'passive',2.54),('2','',5.08,0,180,'passive',2.54)],rect(-2.54,1.016,2.54,-1.016),'R',0,hide_names=True)
array_pins=[]; array_graphics=''
for channel in range(4):
    yy=7.62-channel*5.08
    array_pins += [(str(channel+1),'',-7.62,yy,0,'passive',5.08),(str(8-channel),'',7.62,yy,180,'passive',5.08)]
    array_graphics += rect(-2.54,yy+1.016,2.54,yy-1.016)
make_symbol('R_Array4',array_pins,array_graphics,'RN',10.16,'4 isolated 33-ohm elements; pin pairs 1-8 / 2-7 / 3-6 / 4-5.',True)
make_symbol('C', [('1','',0,5.08,270,'passive',3.81),('2','',0,-5.08,90,'passive',3.81)],line([(-2.54,1.27),(2.54,1.27)])+line([(-2.54,-1.27),(2.54,-1.27)]),'C',0,hide_names=True)
make_symbol('L', [('1','',-7.62,0,0,'passive',2.54),('2','',7.62,0,180,'passive',2.54)],line([(-5.08,0),(-3.81,2.54),(-2.54,0),(-1.27,2.54),(0,0),(1.27,2.54),(2.54,0),(3.81,2.54),(5.08,0)]),'L',0,hide_names=True)
make_symbol('D_Schottky',[('1','K',5.08,0,180,'passive',2.54),('2','A',-5.08,0,0,'passive',2.54)],line([(-2.54,2.54),(-2.54,-2.54),(2.54,0),(-2.54,2.54)])+line([(1.524,2.54),(2.54,2.54),(2.54,-2.54),(3.556,-2.54)]),'D',0,hide_names=True)
make_symbol('SJ2',[('1','',-5.08,0,0,'passive',3.81),('2','',5.08,0,180,'passive',3.81)],rect(-1.27,.762,-.381,-.762)+rect(.381,.762,1.27,-.762),'JP',0,hide_names=True)
make_symbol('SJ3',[('1','A',-5.08,0,0,'passive',3.81),('2','C',0,-5.08,90,'passive',3.81),('3','B',5.08,0,180,'passive',3.81)],rect(-1.27,.762,-.381,-.762)+rect(.381,.762,1.27,-.762)+rect(-.381,-.762,.381,-1.27),'JP',0,hide_names=True)
make_symbol('PWR_FLAG',[('1','pwr',0,0,90,'power_out',0)],line([(0,0),(0,2.54),(-1.27,2.54),(0,3.81),(1.27,2.54),(0,2.54)]),'#FLG',0,hide_names=True)

host={1:'+5V',2:'+5V',3:'GND',4:'GND',5:'GND',6:'H_R3',7:'H_R4',8:'H_R5',9:'H_R6',10:'H_R7',11:'GND',12:'GND',13:'GND',14:'H_G2',15:'H_G3',16:'H_G4',17:'H_G5',18:'H_G6',19:'H_G7',20:'GND',21:'GND',22:'GND',23:'GND',24:'H_B3',25:'H_B4',26:'H_B5',27:'H_B6',28:'H_B7',29:'GND',30:'H_PCLK',31:'H_HSYNC',32:'H_VSYNC',33:'H_DE',34:'H_BL_PWM',35:'CTP_RST',36:'CTP_SDA',37:'H_LCD_READY',38:'CTP_SCL',39:'CTP_INT',40:'H_NRST','MP':'GND'}
ports={6:'PH9/R3',7:'PH10/R4',8:'PH11/R5',9:'PH12/R6',10:'PG6/R7',14:'PH13/G2',15:'PH14/G3',16:'PH15/G4',17:'PI0/G5',18:'PI1/G6',19:'PI2/G7',24:'PG11/B3',25:'PI4/B4',26:'PI5/B5',27:'PI6/B6',28:'PI7/B7',30:'PG7/PCLK',31:'PI10/HSYNC',32:'PI9/VSYNC',33:'PF10/DE',34:'PB5/BL',35:'PI8/T_CS',36:'PI3/T_MOSI',37:'PG3/T_MISO',38:'PH6/T_SCK',39:'PH7/T_PEN',40:'RESET'}
box_symbol('HOST_FPC40',[(i,ports.get(i,host[i]),'passive') for i in range(1,21)]+[('MP','HOLD_DOWN','passive')],[(i,ports.get(i,host[i]),'passive') for i in range(40,20,-1)],19.05,'J','User image 1; 40 contacts, pitch 0.5mm; MP = mounting metal.')

lcd={1:'LCD_SDA',2:'LCD_SCL',3:'LCD_PIN3',4:'LCD_PIN4',5:'GND',6:'LCD_PIN6',7:'LCD_DE',8:'LCD_VSYNC',9:'LCD_HSYNC',10:'LCD_PIN10',11:'LCD_PCLK',12:'GND',37:'+3V3',38:'GND',39:'LEDA',40:'LEDK','MP':'GND'}
for col,start,bits in [('B',20,3),('G',28,2),('R',36,3)]:
    for b in range(8): lcd[start-b]=f'LCD_{col}{b}' if b>=bits else 'GND'
lcdnames={**{i:lcd[i].replace('LCD_','') for i in lcd},3:'P3 / CS or RESET',4:'P4 / RESET or CS',6:'P6 / NC or GND',10:'P10 / NC or VCC',37:'VCC', 'MP':'HOLD_DOWN'}
for c,s in [('B',20),('G',28),('R',36)]:
    for b in range(8):lcdnames[s-b]=f'{c}{b}'
box_symbol('LCD_FPC40',[(i,lcdnames[i],'passive') for i in range(1,21)]+[('MP','HOLD_DOWN','passive')],[(i,lcdnames[i],'passive') for i in range(40,20,-1)],20.32,'J','User images 2/3: labels attached to numbered pins take precedence over mismatched internal pin names.')
lcd_a={**lcd,3:'LCD_RST',4:'LCD_CS',6:None,10:None}
lcd_b={**lcd,3:'LCD_CS',4:'LCD_RST',6:'GND',10:'+3V3'}
for variant,names2 in [('A',{**lcdnames,3:'RESET',4:'CS',6:'NC',10:'NC'}),('B',{**lcdnames,3:'CS',4:'RESET',6:'GND',10:'VCC'})]:
    box_symbol('LCD_'+variant,[(i,names2[i],'passive') for i in range(1,21)]+[('MP','HOLD_DOWN','passive')],[(i,names2[i],'passive') for i in range(40,20,-1)],20.32,'J','Dedicated fixed pinout from user image '+('2' if variant=='A' else '3')+'. Pin numbering must match actual FPC tail.')
box_symbol('CTP_A',[(i,n,'passive') for i,n in enumerate(['VCC','GND','SCL','SDA','INT','RST'],1)]+[('MP','HOLD_DOWN','passive')],[],10.16,'J')
box_symbol('CTP_B',[(i,n,'passive') for i,n in enumerate(['GND','VCC','INT','SCL','SDA','RST'],1)]+[('MP','HOLD_DOWN','passive')],[],10.16,'J')
box_symbol('SWD6',[(i,n,'passive') for i,n in enumerate(['VTREF','GND','SWDIO','SWCLK','NRST','TX_LOG'],1)],[],10.16,'J')
box_symbol('TESTPOINT',[('1','TP','passive')],[],2.54,'TP')
box_symbol('AP2112K_3V3',[('1','VIN','power_in'),('3','EN','input')],[('5','VOUT','power_out'),('2','GND','power_in'),('4','NC','no_connect')],10.16,desc='AP2112K-3.3TRG1, SOT25. Verify LDO thermal budget with actual LCD load.')
box_symbol('SY7200A',[('6','IN','power_in'),('4','EN/PWM','input'),('2','GND','power_in')],[('1','LX','open_collector'),('5','OVP','input'),('3','FB','input')],10.16,desc='SY7200ABC SOT23-6; ILED=0.2V/Rsense.')
box_symbol('LVC1G08',[('1','A','input'),('2','B','input'),('3','GND','power_in')],[('4','Y','output'),('5','VCC','power_in')],10.16,desc='SN74LVC1G08DBVR, SOT23-5; BL_ENABLE = HOST_PWM AND PY_READY.')
box_symbol('PY32F002AW15S',[
 ('4','VCC','power_in'),('13','VSS','power_in'),('8','PF2/NRST','input'),('6','PA13/SWDIO','bidirectional'),('7','PA14/SWCLK','input'),('1','PA7','bidirectional'),('5','PA9/TX_LOG','bidirectional'),('9','PF0','bidirectional')],
 [('11','PA0/LCD_SCK','bidirectional'),('12','PA1/LCD_SDA','bidirectional'),('14','PA2/LCD_CS','bidirectional'),('15','PA3/LCD_RST','bidirectional'),('16','PA6/READY','bidirectional'),('2','PB0','bidirectional'),('3','PB1','bidirectional'),('10','PF1','bidirectional')],17.78,desc='Puya PY32F002AW15S, SOP16 150mil; GPIO SPI supports 8/9 bit protocol when firmware configured.')

manifest=[]
class Sheet:
    def __init__(self,file,title,number):
        self.file=file; self.title=title; self.number=number; self.id=uid(file); self.items=[]; self.used=set()
        self.path='/'+uid(NAME)+('' if number==1 else '/'+uid('sheet/'+file))
    def text(self,s,x,y,size=1.27):
        self.items.append(f'(text {q(s)} (at {f(x)} {f(y)} 0) {effects(size,"(justify left top)")} (uuid {q(uid(self.file+"/text/"+str(len(self.items))))}))')
    def wire(self,x1,y1,x2,y2):
        self.items.append(f'(wire (pts (xy {f(x1)} {f(y1)}) (xy {f(x2)} {f(y2)})) (stroke (width 0) (type default)) (uuid {q(uid(self.file+"/wire/"+str(len(self.items))))}))')
    def label(self,net,x,y,angle=0):
        ident=uid(self.file+'/label/'+str(len(self.items)))
        self.items.append(f'(global_label {q(net)} (shape passive) (at {f(x)} {f(y)} {angle}) {effects(1.016,"(justify left)")} (uuid {q(ident)}) {prop("Intersheetrefs","${INTERSHEET_REFS}",x,y,True)})')
    def comp(self,sym,ref,val,x,y,nets,fp='',rotate=0,notes='',dnp=False,datasheet=''):
        self.used.add(sym); identifier=uid('component/'+ref); h=symbols[sym]['h']
        data={str(k):v for k,v in nets.items()}
        small=sym in ['R','L','SJ2','SJ3','D_Schottky']
        ry=y-(5.08 if small else h+5.08); vy=ry+2.54
        if sym=='C': rx=x+6.35; ry=y-1.27; vx=rx; vy=y+1.27
        else: rx=vx=x
        body=f'(symbol (lib_id {q("Adapter:"+sym)}) (at {f(x)} {f(y)} {rotate}) (unit 1) (in_bom {"no" if ref.startswith("#") else "yes"}) (on_board {"no" if ref.startswith("#") else "yes"}) (dnp {"yes" if dnp else "no"}) (uuid {q(identifier)})'
        for key,value,px,py,hide in [('Reference',ref,rx,ry,ref.startswith('#')),('Value',val,vx,vy,ref.startswith('#')),('Footprint',fp,x,y,True),('Datasheet',datasheet,x,y,True),('Design_Note',notes,x,y,True)]: body+=prop(key,value,px,py,hide,1.016 if small or sym=='C' else 1.27)
        for num,p in symbols[sym]['pins'].items():
            body+=f'(pin {q(num)} (uuid {q(uid(ref+"/pin/"+num))}))'
            _,nm,dx,dy,ang,t,length=p
            rad=math.radians(rotate); xx=x+dx*math.cos(rad)-dy*math.sin(rad); yy=y-(dx*math.sin(rad)+dy*math.cos(rad))
            net=data.get(num)
            if net is None:
                self.items.append(f'(no_connect (at {f(xx)} {f(yy)}) (uuid {q(uid(ref+"/nc/"+num))}))')
            else:
                pa=(ang+rotate)%360
                # Extend away from pin body; label tip meets wire and text extends outward.
                length2=5.08
                ex=xx-length2*math.cos(math.radians(pa)); ey=yy+length2*math.sin(math.radians(pa))
                self.wire(xx,yy,ex,ey)
                self.label(net,ex,ey,pa)
        body+=f'(instances (project {q(NAME)} (path {q(self.path)} (reference {q(ref)}) (unit 1)))))'
        self.items.append(body)
        if not ref.startswith('#'):
            manifest.append({'ref':ref,'value':val,'symbol':sym,'footprint':fp,'nets':data,'uuid':identifier,'sheet_path':self.path,'dnp':dnp,'notes':notes,'datasheet':datasheet,'sheet':self.file})
    def child(self,other,x,y,w=76.2,h=20.32):
        self.items.append(f'(sheet (at {f(x)} {f(y)}) (size {f(w)} {f(h)}) (stroke (width .254) (type default)) (fill (color 0 0 0 0)) (uuid {q(uid("sheet/"+other.file))}) {prop("Sheetname",other.title,x+w/2,y-2.54)} {prop("Sheetfile",other.file+".kicad_sch",x+w/2,y+h+2.54)} (instances (project {q(NAME)} (path {q("/"+uid(NAME))} (page {q(other.number)})))))')
    def save(self):
        lib=''.join(symbols[s]['text'].replace('(symbol '+q(s),'(symbol '+q('Adapter:'+s),1) for s in sorted(self.used))
        header=f'(kicad_sch (version 20250114) (generator "eeschema") (uuid {q(self.id)}) (paper "A3") (title_block (title {q(self.title)}) (date "2026-09-23") (rev "0.1-DRAFT") (company "H743 RGB Adapter") (comment 1 "Electrical draft - panel identity, power and FPC direction require confirmation")) (lib_symbols {lib})'
        if self.number==1: header+='(sheet_instances (path "/" (page "1")))'
        (ROOT/(self.file+'.kicad_sch')).write_text(header+'\n'+'\n'.join(self.items)+')\n',encoding='utf8')

fpR='Adapter:R_0603'; fpC='Adapter:C_0603'; fpJ40='Adapter:Omron_XF2M_40'; fpJ6='Adapter:Omron_XF2M_6'
fpJP2='Adapter:SJ2'; fpJP3='Adapter:SJ3'
main=Sheet(NAME,'01 - H743 host and RGB565',1)
panel=Sheet('02_LCD_Touch','02 - LCD selection and touch',2)
power=Sheet('03_Power_Backlight','03 - Power and backlight',3)
mcu=Sheet('04_PY32_Init','04 - PY32 display initialization',4)

main.text('STM32H743 RGB LCD ADAPTER / REV 0.1',15.24,15.24,2.54)
main.text('40-pin 0.5mm host cable -> RGB565 -> two dedicated 40-pin LCD sockets (use ONE)',15.24,22.86,1.524)
main.text('HOST: user image 1. Connector numbering must be checked with the actual FFC cable.',15.24,33.02)
main.comp('HOST_FPC40','J1','H743 / XF2M-4015-1A',66.04,99.06,host,fpJ40,notes='40pin 0.5mm confirmed by user. Verify pin1-to-pin1 continuity; two MP pads are ground.')
main.text('RGB565: R3..R7, G2..G7, B3..B7\nLCD lower bits R0..2/G0..1/B0..2 are held LOW.\nNo RGB888 or missing-bit reconstruction is implied.',15.24,139.7)
main.text('Host touch pins are reassigned:\nPH6 -> software I2C SCL; PI3 -> software I2C SDA\nPH7 -> CTP interrupt; PI8 -> CTP reset\nPG3 <- PY32 READY; PB5 -> backlight PWM\nHost RESET -> optional 0R link -> PY32 NRST',15.24,157.48)
main.text('33R x4 ISOLATED ARRAYS / tune on real cable',165.1,33.02,1.524)
rgb=[f'R{i}' for i in range(3,8)]+[f'G{i}' for i in range(2,8)]+[f'B{i}' for i in range(3,8)]+['PCLK','HSYNC','VSYNC','DE']
for i in range(5):
    x=208.28 if i<3 else 327.66; y=63.5+(i%3)*48.26
    rn_nets={}
    for ch,n in enumerate(rgb[i*4:i*4+4]): rn_nets[ch+1]='H_'+n; rn_nets[8-ch]='LCD_'+n
    main.comp('R_Array4',f'RN{i+1}','33R x4 / CAY16-330J4LF',x,y,rn_nets,'Adapter:R_Array4_0603',notes='4 isolated elements, never commoned. Initial 33R; adapter-side series link is not ideal source termination for long FFC.',datasheet='https://www.bourns.com/docs/Product-Datasheets/CATCAY.pdf')
main.text('RN1..RN4: 16 RGB lines; RN5: PCLK/HSYNC/VSYNC/DE.\nPlace close to J1; leave the RGB corridor clear for later routing.\nUse isolated 0603 x4 (3216 package), not a common-pin array.',165.1,187.96)
main.child(panel,20.32,210.82); main.child(power,154.94,210.82); main.child(mcu,289.56,210.82)
main.text('All pages share explicitly named global nets. Open each block in the KiCad hierarchy.',20.32,243.84)
main.comp('C','C1','100n / 16V',121.92,66.04,{1:'+5V',2:'GND'},fpC)

panel.text('TWO DEDICATED SCREEN SOCKETS / CONNECT ONLY ONE SCREEN',15.24,15.24,2.032)
panel.text('J2 / PROFILE A / user image 2\nOriginal connector: drawer, top contact\nP3=RESET; P4=CS; P6=NC; P10=NC',15.24,30.48,1.524)
panel.text('J7 / PROFILE B / user image 3\nOriginal connector: flip, bottom contact\nP3=CS; P4=RESET; P6=GND; P10=VCC',226.06,30.48,1.524)
panel.comp('LCD_A','J2','SCREEN A / XF2M-4015-1A',76.2,91.44,lcd_a,fpJ40,notes='Dedicated image2 pinout, pins6/10 NC. Candidate dual-contact seat supports top-contact tail; confirm 0.3+/-0.05mm thickness and pin1.')
panel.comp('LCD_B','J7','SCREEN B / XF2M-4015-1A',287.02,91.44,lcd_b,fpJ40,notes='Dedicated image3 pinout, pin6 GND and pin10 3V3. Candidate dual-contact seat supports bottom-contact tail; confirm 0.3+/-0.05mm thickness and pin1.')
panel.text('Fixed wiring, no jumpers. Two pinout families serve five panel models.\nManually flash the PY32 initialization firmware for the installed panel.\nRGB, SPI, reset and backlight are shared: NEVER connect both screens.',15.24,132.08,1.524)
panel.text('Image2 symbol internal names are inconsistent.\nFollow external labels and numbered pins.\nBoth seats use a verified dual-contact footprint;\nmechanical tail compatibility still requires checking.',226.06,132.08,1.27)
panel.comp('C','C2','100n / 10V',63.5,165.1,{1:'+3V3',2:'GND'},fpC)
panel.comp('C','C3','10u / 10V',111.76,165.1,{1:'+3V3',2:'GND'},'Adapter:C_0805')
panel.comp('C','C16','100n / 10V',274.32,165.1,{1:'+3V3',2:'GND'},fpC)
panel.comp('C','C17','10u / 10V',322.58,165.1,{1:'+3V3',2:'GND'},'Adapter:C_0805')
panel.text('TOUCH FPCs / connect ONLY ONE panel touch tail at a time',15.24,180.34,1.778)
ctpa={1:'+3V3',2:'GND',3:'CTP_SCL',4:'CTP_SDA',5:'CTP_INT',6:'CTP_RST','MP':'GND'}
ctpb={1:'GND',2:'+3V3',3:'CTP_INT',4:'CTP_SCL',5:'CTP_SDA',6:'CTP_RST','MP':'GND'}
for ref,sym,x,net in [('J3','CTP_A',71.12,ctpa),('J4','CTP_B',180.34,ctpb),('J5','CTP_A',289.56,ctpa)]:
    panel.comp(sym,ref,'XF2M-0615-1A',x,210.82,net,fpJ6,notes='0.5mm/0.3mm candidate; confirm actual touch tail dimensions/contact direction. Only one of J3/J4/J5 may be used.')
for ref,net,x in [('R1','CTP_SCL',66.04),('R2','CTP_SDA',175.26)]:
    panel.comp('R',ref,'4k7',x,248.92,{1:'+3V3',2:net},fpR,notes='DNP if equivalent pullups already fitted on touch module.')
panel.comp('R','R3','10k',284.48,248.92,{1:'+3V3',2:'CTP_RST'},fpR)
panel.comp('C','C4','100n / 10V',375.92,208.28,{1:'+3V3',2:'GND'},fpC)

power.text('5V INPUT FROM J1 / LOCAL 3.3V LOGIC',15.24,15.24,2.032)
power.comp('AP2112K_3V3','U1','AP2112K-3.3TRG1',91.44,50.8,{1:'+5V',2:'GND',3:'+5V',4:None,5:'+3V3'},'Adapter:SOT23_5',datasheet='https://www.diodes.com/datasheet/download/AP2112.pdf')
for ref,val,x,net,fp in [('C5','10u / 16V',33.02,'+5V','Adapter:C_0805'),('C6','100n / 16V',53.34,'+5V',fpC),('C7','10u / 10V',147.32,'+3V3','Adapter:C_0805'),('C8','100n / 10V',177.8,'+3V3',fpC)]:
    power.comp('C',ref,val,x,83.82,{1:net,2:'GND'},fp)
power.comp('PWR_FLAG','#FLG01','PWR_FLAG',223.52,43.18,{1:'+5V'})
power.comp('PWR_FLAG','#FLG02','PWR_FLAG',223.52,66.04,{1:'GND'})
power.text('LDO power = (5 - 3.3) x I_3V3.\n600mA IC rating is NOT a thermal guarantee.\nValidate LCD + touch + PY32 load and copper area.\nAll LCD/touch logic currently assumes 3.3V.',254,38.1,1.524)
power.text('BACKLIGHT BOOST / I_LED = 0.2V / R4',15.24,119.38,2.032)
power.comp('L','L1','4.7uH',66.04,142.24,{1:'+5V',2:'BL_LX'},'Adapter:L_T113_2.5x2.0',notes='T113_MIPI_Adapter placement/footprint; selected 4.7uH part, verify Isat, DCR and thermal.')
power.comp('D_Schottky','D1','SS14',147.32,142.24,{1:'LEDA',2:'BL_LX'},'Adapter:D_SMA')
power.comp('SY7200A','U2','SY7200ABC',106.68,185.42,{1:'BL_LX',2:'GND',3:'LEDK',4:'BL_EN',5:'LEDA',6:'+5V'},'Adapter:SOT23_6',datasheet='https://www.silergy.com/download/downloadFile?ftype=note&id=4351&type=product')
power.comp('C','C9','10uF / 50V',35.56,187.96,{1:'+5V',2:'GND'},'Adapter:C_0805')
power.comp('C','C10','1uF / 50V',63.5,213.36,{1:'+5V',2:'GND'},'Adapter:C_0805')
power.comp('C','C11','10uF / 50V',205.74,162.56,{1:'LEDA',2:'GND'},'Adapter:C_0805',notes='T113 backlight output capacitor; 50V X7R, account for DC bias.')
power.comp('R','R4','5R1 / 1%',203.2,200.66,{1:'LEDK',2:'GND'},'Adapter:R_0603',notes='Feedback value retained at 5.1 ohm / 1%; only the T113 0603 body and placement are adopted.')
power.text('LEDK is the sense node, NOT ground.\n39.2mA is provisional; confirm both panels.\nThis boost driver requires LED string voltage above 5V.\nCheck LED series/parallel topology before assembly.',17.78,243.84,1.27)
power.text('BACKLIGHT INTERLOCK',266.7,119.38,1.778)
power.comp('LVC1G08','U4','SN74LVC1G08DBVR',314.96,152.4,{1:'H_BL_PWM',2:'LCD_READY',3:'GND',4:'BL_GATE',5:'+3V3'},'Adapter:SOT23_5',datasheet='https://www.ti.com/lit/ds/symlink/sn74lvc1g08.pdf')
power.comp('R','R5','10k',314.96,185.42,{1:'BL_GATE',2:'BL_EN'},fpR)
power.comp('R','R6','100k',314.96,210.82,{1:'BL_GATE',2:'GND'},fpR)
power.comp('R','R7','100k',314.96,236.22,{1:'H_BL_PWM',2:'GND'},fpR)
power.comp('C','C12','100n / 10V',375.92,180.34,{1:'+3V3',2:'GND'},fpC)
power.text('PY_READY=0 during reset/init -> BL_EN=0.\nRecommended SY7200A PWM: 20kHz..1MHz.\nSelect a valid PB5 timer or use GPIO enable.',254,91.44)

mcu.text('PY32F002A / GPIO SPI INITIALIZER / 3.3V',15.24,15.24,2.032)
mcunets={1:None,2:None,3:None,4:'+3V3',5:'PY_TX_LOG',6:'PY_SWDIO',7:'PY_SWCLK',8:'PY_NRST',9:None,10:None,11:'PY_LCD_SCK',12:'PY_LCD_SDA',13:'GND',14:'PY_LCD_CS',15:'PY_LCD_RST',16:'LCD_READY'}
mcu.comp('PY32F002AW15S','U3','PY32F002AW15S / SOP16',99.06,68.58,mcunets,'Adapter:SOIC16',datasheet='https://www.puyasemi.com/download_path/%E6%95%B0%E6%8D%AE%E6%89%8B%E5%86%8C/MCU%20%E5%BE%AE%E5%A4%84%E7%90%86%E5%99%A8/PY32F002A_Datasheet_V0.2.pdf',notes='Select exact PY32F002AW15S orderable suffix. Other PY32F002 variants/packages have different pinouts.')
for i,(suffix,target) in enumerate([('SCK','LCD_SCL'),('SDA','LCD_SDA'),('CS','LCD_CS'),('RST','LCD_RST')]):
    mcu.comp('R',f'R{8+i}','100R',248.92,45.72+i*22.86,{1:'PY_LCD_'+suffix,2:target},fpR)
mcu.comp('R','R12','10k',248.92,149.86,{1:'+3V3',2:'LCD_CS'},fpR)
mcu.comp('R','R13','10k',248.92,175.26,{1:'+3V3',2:'LCD_RST'},fpR)
mcu.comp('R','R14','100k',248.92,200.66,{1:'LCD_READY',2:'GND'},fpR)
mcu.comp('R','R15','1k',248.92,226.06,{1:'LCD_READY',2:'H_LCD_READY'},fpR,notes='PG3 on host must be INPUT; provides init-ready handshake.')
mcu.comp('R','R16','0R',63.5,121.92,{1:'H_NRST',2:'PY_NRST'},fpR,notes='Host reset link. DNP for isolated MCU debugging; confirm host RESET is 3.3V domain.')
mcu.comp('R','R17','10k',63.5,147.32,{1:'+3V3',2:'PY_NRST'},fpR)
mcu.comp('C','C13','100n / 10V',124.46,129.54,{1:'PY_NRST',2:'GND'},fpC)
mcu.comp('C','C14','100n / 10V',124.46,175.26,{1:'+3V3',2:'GND'},fpC)
mcu.comp('C','C15','1u / 10V',160.02,175.26,{1:'+3V3',2:'GND'},fpC)
mcu.comp('SWD6','J6','PY32 SWD / 2.54mm',66.04,190.5,{1:'+3V3',2:'GND',3:'PY_SWDIO',4:'PY_SWCLK',5:'PY_NRST',6:'PY_TX_LOG'},'Adapter:Header1x06',notes='VTREF is a reference output; do not back-power through debugger. PA9 debug UART is firmware-dependent.')
mcu.text('No model-select jumper or automatic identification.\nGPIO SPI: PA0 clock, PA1 data, PA2 CS, PA3 reset.\nPA6 READY remains LOW until a valid init table completes.\nPA13/PA14 remain dedicated to SWD.\nManually flash the matching firmware for each of five panels.',312.42,38.1)
mcu.text('FIRMWARE CONTRACT / manually flashed per panel\n1. READY=0; CS=1; clock at valid idle level.\n2. Wait for power; issue panel-specific reset timing.\n3. Apply that panel\'s 8/9-bit SPI command table.\n4. Set READY=1 only after all panel delays finish.\n5. Host waits for PG3 before enabling LTDC/backlight.\n\nFive panel models share two connector pinout families.\nNo universal initialization or runtime selection is assumed.\nThis delivery covers hardware only; see docs/.',312.42,93.98,1.27)
for ref,net,x in [('TP1','+5V',337.82),('TP2','+3V3',378.46),('TP3','GND',337.82),('TP4','LCD_READY',378.46)]:
    mcu.comp('TESTPOINT',ref,net,x,193.04 if ref in ['TP1','TP2'] else 223.52,{1:net},'Adapter:TestPoint')

for s in [main,panel,power,mcu]: s.save()
LIB.write_text('(kicad_symbol_lib (version 20250114) (generator "kicad_symbol_editor")\n'+'\n'.join(v['text'] for v in symbols.values())+')\n',encoding='utf8')
(ROOT/'sym-lib-table').write_text('(sym_lib_table (version 7) (lib (name "Adapter")(type "KiCad")(uri "${KIPRJMOD}/Adapter.kicad_sym")(options "")(descr "Local audited adapter symbols")))\n',encoding='utf8')
(ROOT/'fp-lib-table').write_text('(fp_lib_table (version 7) (lib (name "Adapter")(type "KiCad")(uri "${KIPRJMOD}/Adapter.pretty")(options "")(descr "Project-local footprints")))\n',encoding='utf8')
proj={'meta':{'filename':NAME+'.kicad_pro','version':1},'board':{'design_settings':{'rules':{'min_clearance':.127,'min_track_width':.15,'min_via_diameter':.6,'min_through_hole_diameter':.3,'min_copper_edge_clearance':.3},'defaults':{'board_outline_line_width':.05}}},'net_settings':{'classes':[{'name':'Default','clearance':.127,'track_width':.2,'via_diameter':.6,'via_drill':.3,'bus_width':12,'diff_pair_gap':.25,'diff_pair_via_gap':.25,'diff_pair_width':.2,'microvia_diameter':.3,'microvia_drill':.1,'pcb_color':'rgba(0, 0, 0, 0.000)','schematic_color':'rgba(0, 0, 0, 0.000)','wire_width':6}], 'meta':{'version':4}},'schematic':{'annotate_start_num':0},'text_variables':{'DESIGN_STATUS':'DRAFT - UNROUTED - VERIFY PANEL'}}
(ROOT/(NAME+'.kicad_pro')).write_text(json.dumps(proj,indent=2),encoding='utf8')
(ROOT/'docs/design_manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf8')
with (ROOT/'docs/BOM.csv').open('w',newline='',encoding='utf-8-sig') as fobj:
    w=csv.writer(fobj); w.writerow(['Reference','Value','Footprint','DNP','Design_note','Datasheet'])
    for c in manifest: w.writerow([c['ref'],c['value'],c['footprint'],c['dnp'],c['notes'],c['datasheet']])
with (ROOT/'docs/connector_pinout.csv').open('w',newline='',encoding='utf-8-sig') as fobj:
    w=csv.writer(fobj);w.writerow(['Connector','Pin','Signal','Source'])
    for j in ['J1','J2','J7','J3','J4','J5','J6']:
        c=next(c for c in manifest if c['ref']==j)
        for p,n in c['nets'].items():w.writerow([j,p,n,'user images' if j!='J6' else 'new design'])
print(f'Created 4 schematic sheets, {len(manifest)} components and local symbol library.')
