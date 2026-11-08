"""Reflow existing KiCad sheets while preserving component UUIDs and netlist.

Uses the pre-edit backup as a stable input; rerunning cannot progressively
shift symbols. This script changes presentation, never values or footprints.
"""
from pathlib import Path
import math
import uuid
import xml.etree.ElementTree as ET
import sexpdata as sx

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'backups/wired_schematics_20260923_124659'
S = sx.Symbol

def key(v):
    return str(v[0]) if isinstance(v, list) and v else ''
def fields(v, k): return [x for x in v if key(x) == k]
def field(v, k): return next(x for x in v if key(x) == k)
def put(v, k, value):
    old = next((x for x in v if key(x) == k), None)
    if old is not None: v[v.index(old)] = value
    else: v.append(value)
def ident(scope, name): return str(uuid.uuid5(uuid.NAMESPACE_URL, 'h743-connected/'+scope+'/'+name))
def pretty(v, level=0):
    if not isinstance(v, list): return sx.dumps(v)
    if not any(isinstance(x, list) and any(isinstance(y, list) for y in x) for x in v):
        return sx.dumps(v)
    chunks = ['(']
    for i,x in enumerate(v):
        if i:
            chunks.append('\n'+'\t'*(level+1) if isinstance(x,list) else ' ')
        chunks.append(pretty(x,level+1))
    chunks.append(')')
    return ''.join(chunks)
def num(v): return round(v, 4)
def point(x,y): return [num(x),num(y)]
def read(path): return sx.loads(path.read_text(encoding='utf-8'))
def write(path,v): path.write_text(pretty(v)+'\n',encoding='utf-8')

nets={}
for net in ET.parse(BASE/'baseline.xml').getroot().findall('nets/net'):
    for n in net.findall('node'):
        nets[(n.attrib['ref'],n.attrib['pin'])]=net.attrib['name']

class Page:
    def __init__(self,name):
        self.name=name
        self.tree=read(BASE/(name+'.kicad_sch'))
        self.symbols={}
        for v in fields(self.tree,'symbol'):
            ref=next(p[2] for p in fields(v,'property') if p[1]=='Reference')
            self.symbols[ref]=v
        self.libs={v[1]:v for v in fields(field(self.tree,'lib_symbols'),'symbol')}
        self.tree[:]=[v for v in self.tree if key(v) not in {'wire','global_label','label','junction','no_connect','text'}]
        self.used=set()
        self.counter=0
    def lib(self,ref): return self.libs[field(self.symbols[ref],'lib_id')[1]]
    def pins(self,ref):
        return {field(p,'number')[1]:p for sub in fields(self.lib(ref),'symbol') for p in fields(sub,'pin')}
    def pos(self,ref,pin):
        p=self.pins(ref)[str(pin)]; a=field(p,'at')
        loc=field(self.symbols[ref],'at'); rad=math.radians(loc[3])
        return tuple(point(loc[1]+a[1]*math.cos(rad)-a[2]*math.sin(rad),loc[2]-a[1]*math.sin(rad)-a[2]*math.cos(rad)))
    def move(self,ref,x,y,angle=None):
        c=self.symbols[ref]; at=field(c,'at'); dx=x-at[1]; dy=y-at[2]
        at[1:3]=point(x,y)
        if angle is not None: at[3]=angle
        for p in fields(c,'property'):
            loc=field(p,'at'); loc[1:3]=point(loc[1]+dx,loc[2]+dy)
        if angle is not None and ref.startswith('R'):
            self.prop(ref,'Reference',x+5.08,y-1.27)
            self.prop(ref,'Value',x+5.08,y+1.27)
    def prop(self,ref,name,x,y):
        p=next(p for p in fields(self.symbols[ref],'property') if p[1]==name)
        field(p,'at')[1:]=[*point(x,y),0]
    def add(self,v): self.tree.append(v)
    def uid(self):
        self.counter+=1
        return [S('uuid'),ident(self.name,str(self.counter))]
    def wire(self,*pts):
        for a,b in zip(pts,pts[1:]):
            if a==b: continue
            self.add([S('wire'),[S('pts'),[S('xy'),*a],[S('xy'),*b]], [S('stroke'),[S('width'),0],[S('type'),S('default')]],self.uid()])
    def dot(self,p): self.add([S('junction'),[S('at'),*p],[S('diameter'),0],[S('color'),0,0,0,0],self.uid()])
    def label(self,net,p,angle=0,local=False):
        # Keep every named inter-sheet signal a global label.  A local label
        # with the same spelling as a global label produces a noisy KiCad ERC
        # "same_local_global_label" warning and can obscure the intended bus.
        v=[S('global_label'),net,[S('shape'),S('passive')]]
        v += [[S('at'),*p,angle],[S('effects'),[S('font'),[S('size'),1.016,1.016]],[S('justify'),S('left')]],self.uid()]
        if not local:
            v.append([S('property'),'Intersheetrefs','${INTERSHEET_REFS}',[S('at'),*p,0],[S('hide'),S('yes')],[S('effects'),[S('font'),[S('size'),1.27,1.27]]]])
        self.add(v)
    def use(self,ref,*pins): self.used.update((ref,str(p)) for p in pins)
    def line(self,ref,pin,*pts):
        self.use(ref,pin); self.wire(self.pos(ref,pin),*pts)
    def text(self,text,x,y,size=1.27):
        self.add([S('text'),text,[S('at'),x,y,0],[S('effects'),[S('font'),[S('size'),size,size]],[S('justify'),S('left'),S('top')]],self.uid()])
    def finish(self):
        for ref,c in self.symbols.items():
            for no,pin in self.pins(ref).items():
                if (ref,no) in self.used: continue
                xy=self.pos(ref,no); net=nets.get((ref,no))
                if net is None or str(net).startswith('unconnected-('):
                    self.add([S('no_connect'),[S('at'),*xy],self.uid()]); continue
                angle=(field(pin,'at')[3]+field(c,'at')[3])%360
                r=math.radians(angle)
                end=tuple(point(xy[0]-5.08*math.cos(r),xy[1]+5.08*math.sin(r)))
                self.wire(xy,end); self.label(net,end,angle)
        write(ROOT/(self.name+'.kicad_sch'),self.tree)


def root_page():
    p=Page('H743_RGB_Adapter')
    # A functional connector drawing makes all twenty RGB/timing channels
    # traceable through their physical isolated-array pin pairs in one row.
    p.move('J1',83.82,127.0)
    right=[]
    for rn in ['RN1','RN2','RN3','RN4','RN5']:
        right.extend(nets[(rn,str(i))] for i in range(1,5))
    ypos={net:60.96+(i//4)*25.4+(i%4)*5.08 for i,net in enumerate(right)}
    ground=['3','4','5','11','12','13','20','21','22','23','29','MP']
    gy=[78.74,81.28,83.82,91.44,93.98,96.52,104.14,106.68,109.22,111.76,114.3,116.84]
    control=['40','39','38','37','36','35','34']
    p.prop('J1','Reference',83.82,33.02)
    p.prop('J1','Value',83.82,36.83)
    for i,rn in enumerate(['RN1','RN2','RN3','RN4','RN5']):
        p.move(rn,187.96,68.58+i*25.4)
        for no in range(1,5):
            net=nets[(rn,str(no))]
            hn=next(n for n in p.pins('J1') if nets[('J1',n)]==net)
            p.use('J1',hn); p.use(rn,no)
            a,b=p.pos('J1',hn),p.pos(rn,no)
            p.wire(a,b); p.label(net,(137.16,a[1]),local=True)
    p.move('C1',27.94,66.04)
    p.use('J1','1','2',*ground); p.use('C1','1','2')
    p.wire((27.94,50.8),(43.18,50.8),(43.18,60.96),p.pos('J1','2'))
    p.wire(p.pos('C1','1'),(27.94,50.8))
    p.wire(p.pos('J1','1'),(43.18,58.42)); p.dot((43.18,58.42))
    p.label('+5V',(27.94,50.8),90)
    p.wire((43.18,78.74),(43.18,124.46),(17.78,124.46),(17.78,71.12),p.pos('C1','2'))
    for no in ground:
        x,y=p.pos('J1',no); p.wire((x,y),(43.18,y))
        if y!=78.74: p.dot((43.18,y))
    p.label('GND',(43.18,124.46),270)
    # Keep the original hierarchy UUIDs, but use the clear right-hand column.
    for i,sh in enumerate(fields(p.tree,'sheet')):
        at=field(sh,'at'); dx=284.48-at[1]; dy=(139.7+i*35.56)-at[2]
        at[1:3]=point(at[1]+dx,at[2]+dy)
        for pr in fields(sh,'property'):
            a=field(pr,'at'); a[1:3]=point(a[1]+dx,a[2]+dy)
    p.text('STM32H743 RGB LCD ADAPTER / HOST + 33R ARRAYS',15.24,15.24,2.032)
    p.text('J1 is drawn by FUNCTION; physical FPC contact numbers are unchanged.',15.24,22.86,1.27)
    p.text('RGB565 / 20 SERIES CHANNELS',266.7,40.64,1.778)
    p.text('RN1..RN4: R3..R7, G2..G7, B3..B7\nRN5: PCLK / HSYNC / VSYNC / DE\nFour isolated 33R elements per package.\n\nPlace near J1. Tune resistance with the real cable.\nLCD unused low bits are held at GND.\nUse one LCD socket and one touch socket.',266.7,50.8)
    p.text('HOST CONTROL SIGNALS',266.7,93.98,1.778)
    p.text('PH6: software I2C SCL; PI3: software I2C SDA\nPH7: touch INT; PI8: touch RESET\nPG3: PY32 READY input; PB5: backlight PWM\nHost RESET links to PY32 NRST through R16.',266.7,104.14)
    p.text('40-pin / 0.5mm. Verify actual cable pin-1 continuity before assembly.',15.24,228.6)
    p.finish()


def lcd_page():
    p=Page('02_LCD_Touch')
    p.text('TWO LCD PINOUT FAMILIES / CONNECT ONE SCREEN',15.24,15.24,2.032)
    p.text('J2 / SCREEN A / image 2\nDrawer / top contact: P3=RESET, P4=CS\nP6 and P10 are NC.',15.24,30.48,1.524)
    p.text('J7 / SCREEN B / image 3\nFlip / bottom contact: P3=CS, P4=RESET\nP6=GND; P10=VCC.',226.06,30.48,1.524)
    for j,c1,c2,x in [('J2','C2','C3',157.48),('J7','C16','C17',368.3)]:
        p.use(j,'37'); p.use(c1,'1','2'); p.use(c2,'1','2')
        a,b=p.pos(c1,'1'),p.pos(c2,'1')
        p.wire(p.pos(j,'37'),(x,73.66),(x,160.02),a,b)
        # Split overlapping rail into a single continuous line.
        p.tree.pop() # last a->b segment was inside x->a; replace no duplicate
        p.dot(b)
        p.label('+3V3',(x,73.66),180)
        a,b=p.pos(c1,'2'),p.pos(c2,'2'); p.wire(a,b)
        p.label('GND',a,0)
        # Adjacent low RGB bits and mount metal form a visible local ground bus.
        ground=[n for n in ['17','18','19','20','MP'] if nets.get((j,n))=='GND']
        gx=p.pos(j,ground[0])[0]-5.08
        yy=[p.pos(j,n)[1] for n in ground]
        p.wire((gx,min(yy)),(gx,max(yy)))
        for n in ground:
            a=p.pos(j,n); p.line(j,n,(gx,a[1]))
            if min(yy)<a[1]<max(yy): p.dot((gx,a[1]))
        p.label('GND',(gx,max(yy)),270)
    p.text('Fixed wiring for five panels in two pinout families. Flash the matching PY32 firmware.\nRGB, SPI, reset and backlight are shared: connect ONE LCD and ONE touch tail.',15.24,132.08,1.27)
    p.text('TOUCH / I2C PULL-UPS AND RESET',15.24,180.34,1.778)
    p.move('J3',96.52,218.44)
    for ref,x in [('R1',35.56),('R2',48.26),('R3',60.96)]:
        p.move(ref,x,198.12,270); p.use(ref,'1','2')
        p.wire((x,190.5),p.pos(ref,'1'))
        p.dot((x,190.5))
    p.wire((20.32,190.5),(73.66,190.5)); p.label('+3V3',(20.32,190.5),0)
    p.move('C4',20.32,203.2); p.use('C4','1')
    p.wire(p.pos('C4','1'),(20.32,190.5))
    p.line('J3','1',(73.66,210.82),(73.66,190.5))
    for r,n in [('R1','3'),('R2','4'),('R3','6')]:
        a=p.pos(r,'2'); b=p.pos('J3',n)
        p.use('J3',n); p.wire(a,(a[0],b[1]),b)
        p.label(nets[('J3',n)],(a[0],b[1]),0)
    # RST and I2C wiring is local to J3; alternative pinout sockets retain tags.
    p.text('J3 / VCC GND SCL SDA INT RST',20.32,238.76)
    p.text('J4 / GND VCC INT SCL SDA RST',144.78,238.76)
    p.text('J5 / VCC GND SCL SDA INT RST',254.0,238.76)
    p.text('R1/R2: 4.7k I2C pull-ups. R3: 10k reset pull-up.\nDo not populate duplicate I2C pull-ups if already fitted on the touch module.',15.24,254.0)
    p.finish()


def mcu_page():
    p=Page('04_PY32_Init')
    p.text('PY32F002A / SOFTWARE SPI INITIALIZER / 3.3V',15.24,15.24,2.032)
    for i,(r,pin,y,net) in enumerate([('R8','11',27.94,'PY_LCD_SCK'),('R9','12',43.18,'PY_LCD_SDA'),('R10','14',58.42,'PY_LCD_CS'),('R11','15',73.66,'PY_LCD_RST')]):
        p.move(r,203.2,y); p.use(r,'1','2'); p.use('U3',pin)
        a=p.pos('U3',pin); bend=139.7+i*10.16
        p.wire(a,(bend,a[1]),(bend,y),p.pos(r,'1'))
        p.label(net,(177.8,y),local=True)
        p.wire(p.pos(r,'2'),(279.4,y)); p.label(nets[(r,'2')],(279.4,y),180)
    for r,x,y,outy in [('R12',228.6,50.8,58.42),('R13',254.0,66.04,73.66)]:
        p.move(r,x,y,270); p.use(r,'1','2')
        p.label('+3V3',p.pos(r,'1'),0)
        p.wire(p.pos(r,'2'),(x,outy)); p.dot((x,outy))
    # Reset conditioning, visibly connected to U3 NRST.
    p.move('R16',50.8,116.84)
    p.move('R17',76.2,101.6,270)
    p.move('C13',116.84,121.92)
    p.use('R16','2'); p.use('R17','2'); p.use('C13','1'); p.use('U3','8')
    p.wire(p.pos('U3','8'),(55.88,64.77),(55.88,116.84),p.pos('C13','1'))
    p.wire(p.pos('R17','2'),(76.2,116.84)); p.dot((76.2,116.84))
    p.label('PY_NRST',(91.44,116.84),local=True)
    # Local MCU supply rail and its capacitor pair.
    for r,x in [('C14',25.4),('C15',43.18)]:
        p.move(r,x,48.26); p.use(r,'1','2')
        p.wire(p.pos(r,'1'),(x,38.1))
    p.use('U3','4')
    p.wire((25.4,38.1),(63.5,38.1),(63.5,59.69),p.pos('U3','4'))
    p.dot((43.18,38.1)); p.label('+3V3',(25.4,38.1),0)
    p.wire(p.pos('C14','2'),p.pos('C15','2')); p.label('GND',p.pos('C14','2'),0)
    # READY: common line, pull-down branch, host protection resistor.
    p.move('R15',203.2,152.4); p.move('R14',160.02,162.56,270)
    p.use('U3','16'); p.use('R14','1'); p.use('R15','1')
    p.wire(p.pos('U3','16'),(132.08,69.85),(132.08,152.4),p.pos('R15','1'))
    p.wire(p.pos('R14','1'),(160.02,152.4)); p.dot((160.02,152.4))
    p.label('LCD_READY',(172.72,152.4),local=True)
    p.text('RESET / R16 links host reset to PY32',20.32,88.9)
    p.text('READY TO HOST + BACKLIGHT GATE',139.7,137.16,1.524)
    p.text('SWD PROGRAMMING',20.32,168.91,1.524)
    p.text('No jumpers; manually flash the selected panel firmware.\nPA0=SCK, PA1=SDA, PA2=CS, PA3=RESET.\nKeep PA13/PA14 available for SWD programming.',312.42,38.1)
    p.text('FIRMWARE SEQUENCE\n1. READY=0; CS=1; clock at panel idle level.\n2. Wait for power, then reset the panel.\n3. Send the panel-specific 8/9-bit SPI table.\n4. Wait all required delays, then set READY=1.\n5. H743 waits for PG3 before backlight enable.\n\nFive panels share two connector pinout families.\nThis project contains hardware only.',312.42,93.98)
    p.text('R14 keeps READY low during reset.\nR15 isolates the host READY input.',139.7,185.42)
    p.finish()

if __name__=='__main__':
    # The root page keeps its existing cross-page/global-label presentation:
    # long host-to-array runs are much clearer as named nets until placement
    # and routing are finalized.  The two child pages contain the local
    # connections this pass can make unambiguously.
    lcd_page(); mcu_page()
    print('Reflowed LCD/touch and PY32 pages; component identities preserved.')
