'use strict';
const plugPresets = [
  { name: 'Dial Codeset 1',  manufacturer: 'Dial',  delay: 410, codes: [ 1381717,  1381716,  1394005,  1394004,  1397077,  1397076,  1397845,  1397844] },
  { name: 'Dial Codeset 2',  manufacturer: 'Dial',  delay: 410, codes: [ 4527445,  4527444,  4539733,  4539732,  4542805,  4542804,  4543573,  4543572] },
  { name: 'Dial Codeset 3',  manufacturer: 'Dial',  delay: 410, codes: [ 5313877,  5313876,  5326165,  5326164,  5329237,  5329236,  5330005,  5330004] },
  { name: 'Dial Codeset 4',  manufacturer: 'Dial',  delay: 410, codes: [ 5510485,  5510484,  5522773,  5522772,  5525845,  5525844,  5526613,  5526612] },
  { name: 'Codeset 3a',      manufacturer: 'Dial',  delay: 410, codes: [10958860, 10958852, 10958858, 10958850, 10958857, 10958849, 10958861, 10958853] },
  { name: 'Codeset 2a',      manufacturer: 'Dial',  delay: 410, codes: [ 1070387,  1070396,  1070531,  1070540,  1070851,  1070860,  1072387,  1072396] },
  { name: 'Codeset 4 alt',   manufacturer: 'Dial',  delay: 410, codes: [12068364, 12068356, 12068362, 12068354, 12068363, 12068355, 12068365, 12068357] },
  { name: 'ZAP USA 0308',    manufacturer: 'ZAP',   delay: 160, codes: [ 4281651,  4281660,  4282243,  4282252,  4281795,  4281804,  4282115,  4282124] },
  { name: 'ZAP USA 0313',    manufacturer: 'ZAP',   delay: 160, codes: [ 1332531,  1332540,  1332675,  1332684,  1332995,  1333004,  1334531,  1334540] },
  { name: 'Arlec Code 1',    manufacturer: 'Arlec', delay: 160, codes: [ 4461875,  4461884,  4462019,  4462028,  4462339,  4462348,  4463875,  4463884] },
  { name: 'Arlec Code 2',    manufacturer: 'Arlec', delay: 160, codes: [ 5526835,  5526844,  5526979,  5526988,  5527299,  5527308,  5528835,  5528844] },
  { name: 'Arlec Code 3',    manufacturer: 'Arlec', delay: 160, codes: [16468316, 15835788, 16028133, 16395413, 16148014, 16569774, 15742919, 16710407] }
];
