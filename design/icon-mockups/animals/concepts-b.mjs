// Distinct animal silhouettes drawn as large folded-paper shapes.
export default [
  {
    id: '05',
    stem: '05-bear',
    name: 'Bear',
    description: 'A broad teal bear with round ears and an ivory muzzle.',
    defs: `
      <linearGradient id="bear-fur" x1="0" y1="0" x2=".7" y2="1"><stop stop-color="#378f99"/><stop offset="1" stop-color="#164a59"/></linearGradient>
      <linearGradient id="bear-muzzle" x1="0" y1="0" x2=".5" y2="1"><stop stop-color="#f5ebd4"/><stop offset="1" stop-color="#d6c8aa"/></linearGradient>
    `,
    art: `
      <path d="M23 61Q14 28 43 19Q72 9 86 43L75 86L36 88Z" fill="#215a67"/>
      <path d="M233 61Q242 28 213 19Q184 9 170 43L181 86L220 88Z" fill="#174651"/>
      <path d="M37 56Q32 33 49 31Q66 27 72 50L63 67L44 68Z" fill="#79b8b2"/>
      <path d="M219 56Q224 33 207 31Q190 27 184 50L193 67L212 68Z" fill="#57958f"/>
      <path d="M47 66L88 43L128 35L168 43L209 66L232 109L227 165L201 202L158 227H98L55 202L29 165L24 109Z" fill="url(#bear-fur)"/>
      <path d="M47 66L88 43L128 35V93L77 110L24 109Z" fill="#449eaa"/>
      <path d="M128 35L168 43L209 66L232 109L178 111L128 93Z" fill="#2a7b88"/>
      <path d="M24 109L77 110L75 167L55 202L29 165Z" fill="#2a7280"/>
      <path d="M232 109L178 111L181 167L201 202L227 165Z" fill="#124450"/>
      <path d="M80 149L105 129H151L176 149L188 179L166 207L128 218L90 207L68 179Z" fill="url(#bear-muzzle)"/>
      <path d="M80 149L105 129H128V174L90 207L68 179Z" fill="#f6ecd6"/>
      <path d="M151 129L176 149L188 179L166 207L128 174V129Z" fill="#e5d7b9"/>
      <path d="M96 145L112 137H144L160 145L150 166L128 178L106 166Z" fill="#17333b"/>
      <path d="M96 145L112 137H144L160 145L128 153Z" fill="#35535a"/>
      <path d="M128 177V191L116 198M128 191L140 198" fill="none" stroke="#795f4d" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M63 112L84 107L96 115L88 126H72Z" fill="#12363e"/>
      <path d="M193 112L172 107L160 115L168 126H184Z" fill="#102e36"/>
      <circle cx="81" cy="115" r="3.2" fill="#e8f9f1"/>
      <circle cx="175" cy="115" r="3.2" fill="#e8f9f1"/>
      <path d="M55 202L90 207L128 218L166 207L201 202L158 227H98Z" fill="#133d49"/>
    `,
  },
  {
    id: '06',
    stem: '06-snow-leopard',
    name: 'Snow leopard',
    description: 'A broad snow leopard with rounded ears, ice-gray facets and amber eyes.',
    defs: `
      <linearGradient id="leopard-fur" x1="0" y1="0" x2=".7" y2="1"><stop stop-color="#e5f0f2"/><stop offset="1" stop-color="#87a4b7"/></linearGradient>
      <linearGradient id="leopard-muzzle" x1="0" y1="0" x2=".7" y2="1"><stop stop-color="#fffef4"/><stop offset="1" stop-color="#dbe5e4"/></linearGradient>
    `,
    art: `
      <path d="M28 69Q14 33 42 20Q72 7 88 44L74 84Z" fill="#668799"/>
      <path d="M228 69Q242 33 214 20Q184 7 168 44L182 84Z" fill="#4c6b81"/>
      <path d="M42 58Q32 36 49 31Q65 25 74 47L64 63Z" fill="#294457"/>
      <path d="M214 58Q224 36 207 31Q191 25 182 47L192 63Z" fill="#263d53"/>
      <path d="M42 71L84 44L128 36L172 44L214 71L234 117L226 170L193 208L154 232H102L63 208L30 170L22 117Z" fill="url(#leopard-fur)"/>
      <path d="M42 71L84 44L128 36V100L72 114L22 117Z" fill="#eaf3f1"/>
      <path d="M128 36L172 44L214 71L234 117L184 114L128 100Z" fill="#c4d9df"/>
      <path d="M22 117L72 114L74 162L63 208L30 170Z" fill="#a8c2cb"/>
      <path d="M234 117L184 114L182 162L193 208L226 170Z" fill="#708fa3"/>
      <path d="M108 49H137L149 62L137 77H115L103 64Z" fill="#344f66"/>
      <path d="M57 74L75 63L92 70L87 89L67 92L54 85Z" fill="#446279"/>
      <path d="M182 64L202 77L201 92L181 94L168 79Z" fill="#3c5a71"/>
      <path d="M35 136L51 125L64 139L60 160L43 166Z" fill="#385b73"/>
      <path d="M221 136L205 125L192 139L196 160L213 166Z" fill="#2e4b63"/>
      <path d="M60 110L85 103L108 117L95 138L73 135Z" fill="#365169"/>
      <path d="M196 110L171 103L148 117L161 138L183 135Z" fill="#2e465e"/>
      <path d="M70 116L87 112L100 120L90 129H77Z" fill="#dfa850"/>
      <path d="M186 116L169 112L156 120L166 129H179Z" fill="#dfa850"/>
      <path d="M82 113H88L91 128H81Z" fill="#18303e"/>
      <path d="M168 113H174L175 128H165Z" fill="#18303e"/>
      <path d="M110 105H146L157 154L128 177L99 154Z" fill="#d8e7e7"/>
      <path d="M110 105H128V177L99 154Z" fill="#f1f7ef"/>
      <path d="M80 150L102 146L128 161L154 146L176 150L191 174L176 197L151 214H105L80 197L65 174Z" fill="url(#leopard-muzzle)"/>
      <path d="M104 151L128 144L152 151L142 169L128 177L114 169Z" fill="#5d5660"/>
      <path d="M104 151L128 144L152 151L128 158Z" fill="#8b7580"/>
      <path d="M128 176V189L112 195M128 189L144 195" fill="none" stroke="#475364" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M80 197L105 214H151L176 197L154 232H102Z" fill="#617f96"/>
    `,
  },
  {
    id: '07',
    stem: '07-rabbit',
    name: 'Rabbit',
    description: 'A folded blue-gray rabbit with one upright ear and one gently bent ear.',
    defs: `
      <linearGradient id="rabbit-fur" x1="0" y1="0" x2=".8" y2="1"><stop stop-color="#d9e5ed"/><stop offset="1" stop-color="#7595ae"/></linearGradient>
      <linearGradient id="rabbit-ear" x1="0" y1="0" x2=".7" y2="1"><stop stop-color="#e7b6a6"/><stop offset="1" stop-color="#ba8275"/></linearGradient>
    `,
    art: `
      <path d="M71 132L43 56L47 20L64 14L80 31L102 116Z" fill="#99b4c8"/>
      <path d="M47 20L64 14L80 31L102 116L84 100L65 39Z" fill="#e4edf1"/>
      <path d="M58 34L69 36L88 109L74 108L55 57Z" fill="url(#rabbit-ear)"/>
      <path d="M151 119L161 46L181 17L212 23L222 50L205 73L187 105L184 135Z" fill="#7e9db7"/>
      <path d="M161 46L181 17L212 23L191 48L173 120L151 119Z" fill="#d1e1e9"/>
      <path d="M181 17L212 23L222 50L201 46Z" fill="#edf3f3"/>
      <path d="M175 50L187 40L195 50L176 116L163 118Z" fill="url(#rabbit-ear)"/>
      <path d="M191 48L212 23L222 50L205 73Z" fill="#7997ac"/>
      <path d="M80 106L117 92L155 100L186 122L204 166L201 195L172 223L130 240L87 227L54 204L45 169L56 133Z" fill="url(#rabbit-fur)"/>
      <path d="M80 106L117 92L155 100L130 148L78 158L56 133Z" fill="#dfe9ec"/>
      <path d="M155 100L186 122L204 166L171 161L130 148Z" fill="#b4cbd8"/>
      <path d="M45 169L78 158L92 193L87 227L54 204Z" fill="#a5bccd"/>
      <path d="M204 166L171 161L161 194L172 223L201 195Z" fill="#6889a4"/>
      <path d="M87 174L108 168L130 180L151 168L174 177L178 199L156 219L130 227L104 218L81 199Z" fill="#f6f3e8"/>
      <path d="M130 180L151 168L174 177L178 199L156 219L130 227Z" fill="#e0e4da"/>
      <path d="M112 179L130 173L148 179L130 195Z" fill="#bc8579"/>
      <path d="M130 194V204L119 209M130 204L141 209" fill="none" stroke="#6d7581" stroke-width="4" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M79 146L96 141L105 151L99 166L84 166L76 156Z" fill="#254256"/>
      <path d="M177 147L162 142L153 152L159 167L173 167L181 157Z" fill="#254256"/>
      <circle cx="91" cy="149" r="3.3" fill="#f4faff"/>
      <circle cx="166" cy="150" r="3.3" fill="#f4faff"/>
      <path d="M87 227L104 218L130 227L156 219L172 223L130 240Z" fill="#7394ae"/>
    `,
  },
  {
    id: '08',
    stem: '08-squirrel',
    name: 'Squirrel',
    description: 'A copper squirrel in profile with a broad curled tail and a pale chest.',
    defs: `
      <linearGradient id="squirrel-tail" x1="0" y1="0" x2=".8" y2="1"><stop stop-color="#f5b14f"/><stop offset=".5" stop-color="#d87c34"/><stop offset="1" stop-color="#9c4f2b"/></linearGradient>
      <linearGradient id="squirrel-body" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#e4954d"/><stop offset="1" stop-color="#b75f32"/></linearGradient>
    `,
    art: `
      <path d="M110 218Q44 226 24 180Q10 150 28 119Q13 93 23 61Q31 28 69 20Q107 12 126 41Q144 67 126 91Q114 107 97 101Q79 97 80 80Q82 65 94 66Q79 52 67 65Q48 84 68 106Q86 126 94 148Q104 178 120 186Z" fill="url(#squirrel-tail)"/>
      <path d="M23 61Q31 28 69 20Q107 12 126 41L91 42L57 55L39 87L28 119Q13 93 23 61Z" fill="#f6b95b"/>
      <path d="M126 41Q144 67 126 91Q114 107 97 101Q79 97 80 80Q82 65 94 66Q80 52 67 65L91 42Z" fill="#bd632d"/>
      <path d="M28 119L68 106L94 148L78 178L24 180Q10 150 28 119Z" fill="#e99b4a"/>
      <path d="M24 180L78 178L110 218Q44 226 24 180Z" fill="#ad562b"/>
      <path d="M122 139L127 102L143 83L169 82L190 104L185 132L202 163L194 199L165 222H114L94 206L91 177Z" fill="url(#squirrel-body)"/>
      <path d="M143 91L133 63L143 33L162 61L164 86Z" fill="#b4673a"/>
      <path d="M143 33L162 61L164 86L151 75L145 53Z" fill="#e9a55d"/>
      <path d="M144 60L151 54L156 78L146 77Z" fill="#754735"/>
      <path d="M174 93L179 61L193 44L200 72L194 101Z" fill="#c47b43"/>
      <path d="M129 107L145 85L174 80L195 94L203 114L233 126L225 141L197 146L177 137L146 144Z" fill="#e29a50"/>
      <path d="M145 85L174 80L195 94L171 105L129 107Z" fill="#f2b363"/>
      <path d="M171 105L195 94L203 114L233 126L202 130L177 137Z" fill="#d58543"/>
      <path d="M224 122L238 128L229 139L222 136Z" fill="#384c51"/>
      <path d="M182 111L188 105L197 110L194 121L186 123L181 117Z" fill="#233d49"/>
      <circle cx="189" cy="111" r="2.3" fill="#f7fbf1"/>
      <path d="M197 146L177 137L167 153L166 193L181 211L194 199L202 163Z" fill="#f7dfb3"/>
      <path d="M167 153L150 160L146 185L166 193Z" fill="#ebc78f"/>
      <path d="M152 148L159 160L186 168L195 162L207 167L200 178L182 182L153 176L136 160Z" fill="#b46734"/>
      <path d="M110 159L133 151L156 170L159 194L138 218H108L94 206L91 177Z" fill="#d18343"/>
      <path d="M110 159L133 151L156 170L124 185L91 177Z" fill="#eaa456"/>
      <path d="M124 185L156 170L159 194L138 218H108Z" fill="#b86a35"/>
      <path d="M139 206L159 211L181 217L203 221L204 231H113L105 222Z" fill="#7f4c32"/>
      <path d="M139 206L159 211L181 217L203 221H127L113 231L105 222Z" fill="#cd8849"/>
    `,
  },
];
