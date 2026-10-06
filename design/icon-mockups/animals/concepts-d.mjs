// Broad silhouettes and a few folded planes keep these animals readable as icons.
export default [
  {
    id: '13', stem: '13-elephant', name: 'Elephant',
    description: 'Wide folded ears and a curled trunk give this elephant a calm, friendly character.',
    defs: `<linearGradient id="elephant-main" x2=".8" y2="1"><stop stop-color="#86c6db"/><stop offset="1" stop-color="#36718b"/></linearGradient><linearGradient id="elephant-ear" x2="1" y2="1"><stop stop-color="#b5dbe3"/><stop offset="1" stop-color="#6295af"/></linearGradient>`,
    art: `<path d="M96 61Q59 20 25 43L13 103L34 170L77 185L102 140Z" fill="#356a8a"/>
      <path d="M96 57Q59 16 25 39L13 99L34 166L77 181L102 136Z" fill="url(#elephant-ear)"/>
      <path d="M160 61Q197 20 231 43L243 103L222 170L179 185L154 140Z" fill="#285c7b"/>
      <path d="M160 57Q197 16 231 39L243 99L222 166L179 181L154 136Z" fill="url(#elephant-main)"/>
      <path d="M25 39L63 57L77 181L34 166L48 107Z" fill="#8db8c8"/>
      <path d="M231 39L193 57L179 181L222 166L207 107Z" fill="#6b9cb3"/>
      <path d="M63 57L93 67L87 131L77 181L48 107Z" fill="#c4dce1"/>
      <path d="M193 57L163 67L169 131L179 181L207 107Z" fill="#a5cbd5"/>
      <path d="M88 69L108 42H148L170 69L180 125L154 166H102L76 125Z" fill="url(#elephant-main)"/>
      <path d="M108 42H148L151 95L128 130L105 95Z" fill="#a2cfd9"/>
      <path d="M88 69L105 95L114 151L102 166L76 125Z" fill="#6096b2"/>
      <path d="M148 42L170 69L180 125L154 166L140 147L151 95Z" fill="#3f7997"/>
      <path d="M92 149L79 191Q107 188 114 155Z" fill="#fff0d4"/>
      <path d="M156 149L177 184Q155 184 144 156Z" fill="#e7ddc7"/>
      <path d="M111 125H146L145 192Q145 211 160 211Q176 211 180 190L201 195Q194 239 156 238Q111 237 111 192Z" fill="#336881"/>
      <path d="M111 125H142L141 190Q140 219 166 218Q178 217 186 203Q178 229 154 230Q117 230 117 190Z" fill="#6ca7bd"/>
      <path d="M111 125H127V188L117 190Z" fill="#8bc3d1"/>
      <path d="M94 110Q101 103 108 110L107 124Q100 130 94 123Z" fill="#203d51"/>
      <path d="M149 110Q156 103 162 110V123Q155 130 149 124Z" fill="#203d51"/>
      <circle cx="99" cy="111" r="3" fill="#f6ffff"/><circle cx="154" cy="111" r="3" fill="#f6ffff"/>`,
  },
  {
    id: '14', stem: '14-pangolin', name: 'Pangolin',
    description: 'Overlapping copper folds and a long curled tail make an unusual, compact animal mark.',
    defs: `<linearGradient id="pangolin-body" x2=".7" y2="1"><stop stop-color="#d8a874"/><stop offset="1" stop-color="#895849"/></linearGradient><linearGradient id="pangolin-tail" x2="1" y2="1"><stop stop-color="#b98a67"/><stop offset="1" stop-color="#4d495d"/></linearGradient>`,
    art: `<path d="M145 127Q215 119 238 177Q255 221 222 237Q194 249 163 228L121 197L147 177L190 212Q207 226 220 213Q233 196 209 177L169 170Z" fill="url(#pangolin-tail)"/>
      <path d="M168 141L204 153L216 183L186 174Z" fill="#d1a178"/>
      <path d="M204 153L234 182L232 203L216 183Z" fill="#976750"/>
      <path d="M220 213L232 203L224 231L202 236L190 212L205 220Z" fill="#775952"/>
      <path d="M121 197L147 177L168 195L172 218L163 228Z" fill="#aa775b"/>
      <path d="M83 158L76 208H103L111 162Z" fill="#71574f"/>
      <path d="M142 154L153 201L179 204L169 158Z" fill="#634e52"/>
      <path d="M50 135Q58 68 110 50Q160 31 189 82L198 147L166 181L93 183L57 166Z" fill="url(#pangolin-body)"/>
      <path d="M50 135L68 112L94 153L79 176L57 166Z" fill="#c09b73"/>
      <path d="M66 91L92 62L118 90L99 119Z" fill="#edc590"/>
      <path d="M92 62L121 48L142 68L118 90Z" fill="#d9b381"/>
      <path d="M121 48L157 53L174 77L142 91L142 68Z" fill="#c49672"/>
      <path d="M66 91L99 119L86 146L55 124Z" fill="#d7b283"/>
      <path d="M118 90L142 91L158 115L128 145L99 119Z" fill="#b98260"/>
      <path d="M142 91L174 77L189 105L169 137L158 115Z" fill="#d4a477"/>
      <path d="M86 146L99 119L128 145L120 177L93 183Z" fill="#b48665"/>
      <path d="M128 145L169 137L166 181L120 177Z" fill="#966d59"/>
      <path d="M169 137L189 105L198 147L166 181Z" fill="#aa7e5e"/>
      <path d="M67 104Q50 97 38 114L13 145L19 155L67 145L80 130Z" fill="#edcfaa"/>
      <path d="M67 104L80 130L67 145L38 147L52 127Z" fill="#c5a07c"/>
      <path d="M13 145L23 144L19 155L13 153Z" fill="#303e49"/>
      <circle cx="51" cy="122" r="6" fill="#293c49"/><circle cx="49" cy="120" r="2" fill="#fff7e9"/>`,
  },
  {
    id: '15', stem: '15-stag', name: 'Stag',
    description: 'A composed teal stag with broad copper antlers and a simple folded face.',
    defs: `<linearGradient id="stag-face" x2="1" y2="1"><stop stop-color="#67c5c6"/><stop offset="1" stop-color="#25687d"/></linearGradient><linearGradient id="stag-antler" x2=".4" y2="1"><stop stop-color="#e6bf87"/><stop offset="1" stop-color="#a77b51"/></linearGradient>`,
    art: `<path d="M99 88L81 70L70 41L70 17L58 13L58 39L43 27L30 30L55 55L68 63L72 81L49 73L34 58L21 63L43 86L78 95L101 116Z" fill="url(#stag-antler)"/>
      <path d="M157 88L175 70L186 41L186 17L198 13L198 39L213 27L226 30L201 55L188 63L184 81L207 73L222 58L235 63L213 86L178 95L155 116Z" fill="url(#stag-antler)"/>
      <path d="M90 109L49 95L35 102L62 132L100 135Z" fill="#4aa4b0"/>
      <path d="M166 109L207 95L221 102L194 132L156 135Z" fill="#28728a"/>
      <path d="M49 103L62 121L88 124L79 112Z" fill="#c1dcd7"/>
      <path d="M207 103L194 121L168 124L177 112Z" fill="#a3cbc9"/>
      <path d="M90 94L110 81H146L166 94L174 136L153 186L149 218L128 242L107 218L103 186L82 136Z" fill="url(#stag-face)"/>
      <path d="M110 81H146L140 137L128 180L116 137Z" fill="#8bd1cd"/>
      <path d="M90 94L116 137L103 186L82 136Z" fill="#438f9f"/>
      <path d="M166 94L140 137L153 186L174 136Z" fill="#226078"/>
      <path d="M103 186L116 163L128 180L140 163L153 186L149 218L128 234L107 218Z" fill="#e2e1c9"/>
      <path d="M128 180L140 163L153 186L149 218L128 234Z" fill="#bbcfc4"/>
      <path d="M110 207L128 199L146 207L137 220H119Z" fill="#244551"/>
      <path d="M94 140L111 144L108 155L97 151Z" fill="#173c4b"/>
      <path d="M162 140L145 144L148 155L159 151Z" fill="#173c4b"/>
      <circle cx="101" cy="146" r="2.5" fill="#f0fbeb"/><circle cx="155" cy="146" r="2.5" fill="#f0fbeb"/>`,
  },
  {
    id: '16', stem: '16-moth', name: 'Moth',
    description: 'Broad copper wings and blue folded edges form a vivid, balanced silhouette.',
    defs: `<linearGradient id="moth-copper" x2=".6" y2="1"><stop stop-color="#ffd694"/><stop offset="1" stop-color="#c87a49"/></linearGradient><linearGradient id="moth-blue" x2="1" y2="1"><stop stop-color="#54bed0"/><stop offset="1" stop-color="#245683"/></linearGradient>`,
    art: `<path d="M111 113L60 75L14 67L23 132L52 181L108 188L122 152Z" fill="#944f3e"/>
      <path d="M145 113L196 75L242 67L233 132L204 181L148 188L134 152Z" fill="#8d4a3b"/>
      <path d="M114 120L92 166L100 226L72 220L48 193L60 149Z" fill="url(#moth-blue)"/>
      <path d="M142 120L164 166L156 226L184 220L208 193L196 149Z" fill="url(#moth-blue)"/>
      <path d="M111 108L60 70L14 62L23 127L52 176L108 183L122 147Z" fill="url(#moth-copper)"/>
      <path d="M145 108L196 70L242 62L233 127L204 176L148 183L134 147Z" fill="url(#moth-copper)"/>
      <path d="M14 62L65 94L111 108L60 70Z" fill="#ffdfa6"/>
      <path d="M242 62L191 94L145 108L196 70Z" fill="#f3c486"/>
      <path d="M23 127L65 94L83 149L52 176Z" fill="#e6a365"/>
      <path d="M233 127L191 94L173 149L204 176Z" fill="#d59055"/>
      <path d="M52 176L83 149L108 183L89 204Z" fill="#d8dcbd"/>
      <path d="M204 176L173 149L148 183L167 204Z" fill="#c5d4b7"/>
      <path d="M60 75L97 108L84 137L48 107Z" fill="#24617c"/>
      <path d="M196 75L159 108L172 137L208 107Z" fill="#24617c"/>
      <path d="M67 94L82 109L76 119L62 108Z" fill="#9ed6d1"/>
      <path d="M189 94L174 109L180 119L194 108Z" fill="#9ed6d1"/>
      <path d="M122 91L109 56L95 42M134 91L147 56L161 42" fill="none" stroke="#70b0b7" stroke-width="10" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M117 96Q128 87 139 96L144 146L136 187L128 211L120 187L112 146Z" fill="#244c64"/>
      <path d="M117 96Q128 87 128 96V211L120 187L112 146Z" fill="#39788d"/>
      <ellipse cx="128" cy="91" rx="15" ry="18" fill="#436d7d"/>
      <circle cx="120" cy="88" r="4" fill="#162e3f"/><circle cx="136" cy="88" r="4" fill="#162e3f"/>`,
  },
];
