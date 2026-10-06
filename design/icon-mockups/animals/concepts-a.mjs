// Faceted animal marks use broad color areas to preserve their silhouette at small sizes.
export default [
  {
    id: '01',
    stem: '01-puffin',
    name: 'Puffin',
    description: 'An ivory and deep blue puffin with a broad orange beak.',
    defs: `
      <linearGradient id="puffin-blue" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#397c9b"/><stop offset="1" stop-color="#143650"/></linearGradient>
      <linearGradient id="puffin-ivory" x1="0" y1="0" x2=".7" y2="1"><stop stop-color="#fffdef"/><stop offset="1" stop-color="#d5e5df"/></linearGradient>
      <linearGradient id="puffin-bill" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#ffbf55"/><stop offset="1" stop-color="#e5652e"/></linearGradient>
    `,
    art: `
      <path d="M76 194L63 223L37 236H100L111 204Z" fill="#e38235"/>
      <path d="M130 197L134 223L114 235H177L159 223L151 195Z" fill="#f3a644"/>
      <path d="M63 223L80 231H37Z" fill="#ffbd60"/>
      <path d="M134 223L146 230L114 235Z" fill="#ffcf76"/>
      <path d="M74 87L67 53L89 27L130 18L169 34L192 67L187 113L171 131L176 166L165 202L139 222L97 221L63 207L40 215L53 177L50 138Z" fill="url(#puffin-blue)"/>
      <path d="M89 27L130 18L169 34L192 67L162 46L116 40L76 66Z" fill="#4f91a7"/>
      <path d="M99 56L129 42L160 47L182 69L185 111L163 132L132 135L104 119L91 88Z" fill="url(#puffin-ivory)"/>
      <path d="M129 42L160 47L182 69L155 65L119 74L91 88L99 56Z" fill="#fffef5"/>
      <path d="M91 88L104 119L132 135L115 101Z" fill="#a9c5c4"/>
      <path d="M127 131L164 128L176 166L165 202L139 222L108 217L114 178Z" fill="url(#puffin-ivory)"/>
      <path d="M164 128L176 166L165 202L139 222L149 181Z" fill="#bfdbd5"/>
      <path d="M74 108L112 123L129 157L109 195L70 208L53 177L56 138Z" fill="#286488"/>
      <path d="M74 108L112 123L91 162L53 177L56 138Z" fill="#3a86a3"/>
      <path d="M112 123L129 157L109 195L91 162Z" fill="#1e506e"/>
      <path d="M53 177L91 162L109 195L70 208Z" fill="#193e5b"/>
      <path d="M172 70L202 75L236 103L208 128L177 124L180 100Z" fill="url(#puffin-bill)"/>
      <path d="M172 70L187 76L192 104L184 125L177 124L180 100Z" fill="#f8d879"/>
      <path d="M202 75L212 85L215 118L208 128L202 105Z" fill="#be542f"/>
      <path d="M192 104L236 103L208 128L184 125Z" fill="#d47533"/>
      <path d="M186 103L236 103L231 108L188 109Z" fill="#913d27"/>
      <path d="M143 71L157 89L143 103L132 86Z" fill="#dd7d43"/>
      <ellipse cx="146" cy="84" rx="9" ry="11" fill="#132f40"/>
      <circle cx="149" cy="80" r="3" fill="#fffdf1"/>
    `,
  },
  {
    id: '02',
    stem: '02-hummingbird',
    name: 'Hummingbird',
    description: 'A turquoise hummingbird in flight, with a copper throat and plum wing.',
    defs: `
      <linearGradient id="hummingbird-teal" x1="0" y1="0" x2=".8" y2="1"><stop stop-color="#69ddd4"/><stop offset=".5" stop-color="#26adba"/><stop offset="1" stop-color="#186f94"/></linearGradient>
      <linearGradient id="hummingbird-plum" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#a998c9"/><stop offset="1" stop-color="#574a87"/></linearGradient>
      <linearGradient id="hummingbird-copper" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#ffc882"/><stop offset="1" stop-color="#c67453"/></linearGradient>
    `,
    art: `
      <path d="M118 128L107 23L135 42L160 80L161 115Z" fill="url(#hummingbird-plum)"/>
      <path d="M107 23L135 42L144 103Z" fill="#b5a8d8"/>
      <path d="M107 23L144 103L118 128Z" fill="#716193"/>
      <path d="M106 163L48 225L85 208L73 241L128 188L143 151Z" fill="#245379"/>
      <path d="M106 163L48 225L85 208L128 175Z" fill="#4fa4af"/>
      <path d="M85 208L73 241L128 188L128 175Z" fill="#173c61"/>
      <path d="M91 170L114 128L137 102L147 82L163 72L180 78L191 93L187 116L169 130L159 153L137 178L108 193L85 193Z" fill="url(#hummingbird-teal)"/>
      <path d="M147 82L163 72L180 78L191 93L167 87L144 95Z" fill="#8ce4d9"/>
      <path d="M187 109L169 130L159 153L137 178L131 153L151 123L163 107Z" fill="url(#hummingbird-copper)"/>
      <path d="M151 123L163 107L187 109L170 116Z" fill="#ffdab2"/>
      <path d="M137 178L108 193L85 193L111 170L131 153Z" fill="#17798e"/>
      <path d="M135 150L98 142L61 104L19 28L76 56L119 100L149 128Z" fill="url(#hummingbird-teal)"/>
      <path d="M19 28L76 56L119 100L82 94Z" fill="#83e2dc"/>
      <path d="M19 28L82 94L98 142L61 104Z" fill="#3fb9c5"/>
      <path d="M82 94L119 100L149 128L135 150L98 142Z" fill="#1d88ac"/>
      <path d="M19 28L82 94L135 150L110 104Z" fill="#5dd2d3"/>
      <path d="M183 94L244 91L187 107Z" fill="#234a62"/>
      <path d="M183 94L244 91L186 99Z" fill="#67a6b4"/>
      <path d="M151 93L162 87L178 95L173 106L157 107Z" fill="#17778b"/>
      <circle cx="169" cy="96" r="7.5" fill="#102f44"/>
      <circle cx="171" cy="93" r="2.7" fill="#fffef4"/>
    `,
  },
  {
    id: '03',
    stem: '03-otter',
    name: 'Otter',
    description: 'A copper otter with rounded ears, a broad ivory muzzle, and a calm expression.',
    defs: `
      <linearGradient id="otter-fur" x1="0" y1="0" x2=".8" y2="1"><stop stop-color="#d79a67"/><stop offset=".5" stop-color="#b56d45"/><stop offset="1" stop-color="#744734"/></linearGradient>
      <linearGradient id="otter-cream" x1="0" y1="0" x2=".4" y2="1"><stop stop-color="#fff0cf"/><stop offset="1" stop-color="#dfc397"/></linearGradient>
      <linearGradient id="otter-nose" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#5a4238"/><stop offset="1" stop-color="#2e292a"/></linearGradient>
    `,
    art: `
      <path d="M61 46Q42 30 30 45Q18 61 34 84L66 93L84 66Z" fill="#935b3e"/>
      <path d="M195 46Q214 30 226 45Q238 61 222 84L190 93L172 66Z" fill="#85513a"/>
      <path d="M56 50Q42 43 37 55Q34 66 47 74L62 71Z" fill="#e4b381"/>
      <path d="M200 50Q214 43 219 55Q222 66 209 74L194 71Z" fill="#c69168"/>
      <path d="M65 53L95 31Q128 19 161 31L191 53L217 92L230 133Q230 162 208 185L177 212Q153 232 128 233Q103 232 79 212L48 185Q26 162 26 133L39 92Z" fill="url(#otter-fur)"/>
      <path d="M65 53L95 31Q128 19 161 31L191 53L163 67L128 52L93 67Z" fill="#e2ae78"/>
      <path d="M39 92L65 53L93 67L76 112L46 145L26 133Z" fill="#c58152"/>
      <path d="M217 92L191 53L163 67L180 112L210 145L230 133Z" fill="#995d40"/>
      <path d="M93 67L128 52L163 67L153 118L128 140L103 118Z" fill="#c88857"/>
      <path d="M26 133L46 145L79 187L128 220L128 233Q103 232 79 212L48 185Q26 162 26 133Z" fill="#86513a"/>
      <path d="M230 133L210 145L177 187L128 220L128 233Q153 232 177 212L208 185Q230 162 230 133Z" fill="#694334"/>
      <path d="M48 132L79 119L107 122L128 135L149 122L177 119L208 132L199 166L173 193L150 207Q128 220 106 207L83 193L57 166Z" fill="url(#otter-cream)"/>
      <path d="M48 132L79 119L107 122L128 148L102 163L65 153Z" fill="#fff2d6"/>
      <path d="M208 132L177 119L149 122L128 148L154 163L191 153Z" fill="#f2ddba"/>
      <path d="M57 166L65 153L102 163L128 190L106 207L83 193Z" fill="#e3c79e"/>
      <path d="M199 166L191 153L154 163L128 190L150 207L173 193Z" fill="#cdb087"/>
      <path d="M72 103Q86 91 99 101" fill="none" stroke="#75472f" stroke-width="8" stroke-linecap="round"/>
      <path d="M157 101Q171 91 184 103" fill="none" stroke="#71452f" stroke-width="8" stroke-linecap="round"/>
      <ellipse cx="87" cy="110" rx="11" ry="13" fill="#292a2c"/>
      <ellipse cx="169" cy="110" rx="11" ry="13" fill="#292a2c"/>
      <circle cx="84" cy="106" r="3.8" fill="#fff9e6"/>
      <circle cx="166" cy="106" r="3.8" fill="#fff9e6"/>
      <path d="M108 138Q128 129 148 138L145 152L128 163L111 152Z" fill="url(#otter-nose)"/>
      <path d="M115 140Q128 136 141 140" fill="none" stroke="#967567" stroke-width="4" stroke-linecap="round"/>
      <path d="M128 160V173Q114 182 104 172M128 173Q142 182 152 172" fill="none" stroke="#715744" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>
    `,
  },
  {
    id: '04',
    stem: '04-badger',
    name: 'Badger',
    description: 'An ivory badger with long blue facial stripes and a strong tapered face.',
    defs: `
      <linearGradient id="badger-ivory" x1="0" y1="0" x2=".65" y2="1"><stop stop-color="#fffced"/><stop offset="1" stop-color="#ccdeda"/></linearGradient>
      <linearGradient id="badger-blue" x1="0" y1="0" x2=".8" y2="1"><stop stop-color="#386d87"/><stop offset="1" stop-color="#17384e"/></linearGradient>
      <linearGradient id="badger-center" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#fffff1"/><stop offset="1" stop-color="#e6eedb"/></linearGradient>
    `,
    art: `
      <path d="M51 66L33 42Q28 22 45 17Q67 13 82 34L88 59Z" fill="url(#badger-blue)"/>
      <path d="M205 66L223 42Q228 22 211 17Q189 13 174 34L168 59Z" fill="url(#badger-blue)"/>
      <path d="M49 51L41 36Q40 27 48 26L65 37L70 54Z" fill="#b7d4d7"/>
      <path d="M207 51L215 36Q216 27 208 26L191 37L186 54Z" fill="#88b0bd"/>
      <path d="M57 47L93 29L128 35L163 29L199 47L224 88L222 127L204 164L170 200L149 228Q128 243 107 228L86 200L52 164L34 127L32 88Z" fill="url(#badger-ivory)"/>
      <path d="M57 47L93 29L128 35L96 63L58 94L32 88Z" fill="#fffef3"/>
      <path d="M199 47L163 29L128 35L160 63L198 94L224 88Z" fill="#e0eee5"/>
      <path d="M32 88L58 94L76 145L86 200L52 164L34 127Z" fill="#91b6be"/>
      <path d="M224 88L198 94L180 145L170 200L204 164L222 127Z" fill="#6b9aa9"/>
      <path d="M72 40L100 32L114 65L113 111L105 151L119 210L94 191L69 146L55 100L56 68Z" fill="url(#badger-blue)"/>
      <path d="M184 40L156 32L142 65L143 111L151 151L137 210L162 191L187 146L201 100L200 68Z" fill="url(#badger-blue)"/>
      <path d="M72 40L100 32L114 65L94 80L55 100L56 68Z" fill="#477f96"/>
      <path d="M184 40L156 32L142 65L162 80L201 100L200 68Z" fill="#376b84"/>
      <path d="M114 65L128 35L142 65L143 111L151 151L139 204H117L105 151L113 111Z" fill="url(#badger-center)"/>
      <path d="M128 35L142 65L143 111L151 151L139 204H128Z" fill="#dce8d8"/>
      <path d="M65 105L80 95L99 100L96 112L81 117L68 114Z" fill="#a3c4ca"/>
      <path d="M191 105L176 95L157 100L160 112L175 117L188 114Z" fill="#a3c4ca"/>
      <ellipse cx="85" cy="106" rx="9" ry="10" fill="#132c3a"/>
      <ellipse cx="171" cy="106" rx="9" ry="10" fill="#132c3a"/>
      <circle cx="83" cy="103" r="3" fill="#fffef2"/>
      <circle cx="169" cy="103" r="3" fill="#fffef2"/>
      <path d="M105 200L117 191H139L151 200L144 218L128 229L112 218Z" fill="#173447"/>
      <path d="M117 196H138L145 201H110Z" fill="#4e7889"/>
      <path d="M86 200L107 228Q128 243 149 228L170 200L144 218L128 229L112 218Z" fill="#bdd3ce"/>
    `,
  },
];
