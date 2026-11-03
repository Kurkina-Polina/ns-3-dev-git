
Без помехи
```bash
./ns3 run "punct-stand --width=80"
RESULT,80,-1,,1,188.582
```
С помехой
```bash
./ns3 run "punct-stand --width=80 --subch=1"
RESULT,80,1,,1,82.866
```
С помехой и вырезанием
```bash
./ns3 run "punct-stand --width=80 --subch=1 --punct=1"
RESULT,80,1,1,1,121.296
```

Скорости без выкалывания
```bash
python3 tools/run_punct.py --widths 80 --repeats 1
width=80 subch=-1 run=1 ... 188.6 Мбит/с
width=80 subch=0 run=1 ... 0.0 Мбит/с
width=80 subch=1 run=1 ... 82.9 Мбит/с
width=80 subch=2 run=1 ... 126.6 Мбит/с
width=80 subch=3 run=1 ... 126.6 Мбит/с
```

Скорости с выкалыванием.
```bash
python3 tools/run_punct.py --widths 80 --repeats 1 --with-punct
width=80 subch=-1 run=1 ... 188.6 Мбит/с                                            
width=80 subch=0 run=1 ... 0.0 Мбит/с
width=80 subch=1 run=1 ... 121.3 Мбит/с
width=80 subch=2 run=1 ... 121.3 Мбит/с
width=80 subch=3 run=1 ... 121.3 Мбит/с
```
