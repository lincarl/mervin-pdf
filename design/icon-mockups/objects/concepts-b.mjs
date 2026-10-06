// Freestanding object concepts with large shapes that survive desktop icon sizes.
export default [
  {
    id: '05',
    stem: '05-open-book',
    name: 'Open book',
    description: 'An open blue book with ivory pages and a warm bookmark.',
    defs: `
      <linearGradient id="book-cover" x1="0" y1="0" x2=".75" y2="1"><stop stop-color="#40b5ef"/><stop offset="1" stop-color="#1252a3"/></linearGradient>
      <linearGradient id="book-left" x1="0" y1="0" x2="1" y2=".3"><stop stop-color="#fffef7"/><stop offset=".7" stop-color="#f3eedf"/><stop offset="1" stop-color="#cacbbb"/></linearGradient>
      <linearGradient id="book-right" x1="0" y1="0" x2="1" y2="0"><stop stop-color="#f3efe3"/><stop offset=".27" stop-color="#fffef7"/><stop offset="1" stop-color="#f8f7ec"/></linearGradient>
    `,
    art: `
      <path d="M14 65Q76 43 128 75Q180 43 242 65V218Q182 199 141 226Q128 234 115 226Q74 199 14 218Z" fill="#103e7b"/>
      <path d="M14 58Q77 40 128 72Q179 40 242 58V209Q181 191 141 218Q128 226 115 218Q75 191 14 209Z" fill="url(#book-cover)"/>
      <path d="M25 55Q83 39 127 67V209Q79 181 25 198Z" fill="#d3d4c7"/>
      <path d="M231 55Q174 39 129 67V209Q177 181 231 198Z" fill="#c5d2d6"/>
      <path d="M25 45Q83 28 128 58V197Q77 169 25 187Z" fill="url(#book-left)"/>
      <path d="M231 45Q175 28 128 58V197Q178 169 231 187Z" fill="url(#book-right)"/>
      <path d="M128 58V199" fill="none" stroke="#b2b5ac" stroke-width="3"/>
      <path d="M36 63Q78 52 111 70M36 88Q78 77 111 95M36 113Q78 102 111 120" fill="none" stroke="#d6d8ce" stroke-width="5" stroke-linecap="round"/>
      <path d="M146 69Q175 52 210 59M146 94Q177 77 215 86M146 119Q177 102 215 110" fill="none" stroke="#d9ded5" stroke-width="5" stroke-linecap="round"/>
      <path d="M181 38Q192 36 207 38V176L194 166L181 177Z" fill="#cb641c" opacity=".18" transform="translate(3 3)"/>
      <path d="M180 37Q193 35 206 38V172L193 162L180 173Z" fill="#f18b28"/>
      <path d="M180 37Q187 36 193 36V162L180 173Z" fill="#ffaf40"/>
    `,
  },
  {
    id: '06',
    stem: '06-page-fan',
    name: 'Page fan',
    description: 'A fan of bright documents with deep blue edges and a folded corner.',
    defs: `
      <linearGradient id="fan-back" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#4ec2ef"/><stop offset="1" stop-color="#247bb9"/></linearGradient>
      <linearGradient id="fan-middle" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#b9edfa"/><stop offset="1" stop-color="#559fd1"/></linearGradient>
      <linearGradient id="fan-front" x1="0" y1="0" x2=".5" y2="1"><stop stop-color="#fffef9"/><stop offset="1" stop-color="#dfeef2"/></linearGradient>
      <linearGradient id="fan-fold" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#c3e2ec"/><stop offset="1" stop-color="#f7ffff"/></linearGradient>
    `,
    art: `
      <g transform="rotate(-18 126 150)">
        <rect x="39" y="26" width="142" height="191" rx="12" fill="#0e578c"/>
        <rect x="39" y="20" width="142" height="188" rx="12" fill="url(#fan-back)"/>
        <path d="M54 40H160V188H54Z" fill="#d1f1fa"/>
      </g>
      <g transform="rotate(-6 132 144)">
        <rect x="63" y="31" width="144" height="198" rx="12" fill="#226797"/>
        <rect x="63" y="25" width="144" height="195" rx="12" fill="url(#fan-middle)"/>
        <path d="M78 43H190V199H78Z" fill="#eefaff"/>
      </g>
      <g transform="rotate(8 155 140)">
        <path d="M105 41H178L220 83V224Q220 236 208 236H105Q93 236 93 224V53Q93 41 105 41Z" fill="#164f89"/>
        <path d="M105 34H178L220 76V217Q220 229 208 229H105Q93 229 93 217V46Q93 34 105 34Z" fill="#278ac4"/>
        <path d="M102 43H176L211 78V211Q211 220 202 220H110Q102 220 102 212Z" fill="url(#fan-front)"/>
        <path d="M177 43L211 78V86H188Q177 86 177 75Z" fill="#79abc1" opacity=".25"/>
        <path d="M177 43L211 78H186Q177 78 177 69Z" fill="url(#fan-fold)"/>
        <path d="M121 112H192" stroke="#247eaf" stroke-width="12" stroke-linecap="round"/>
        <path d="M121 142H187M121 164H187M121 186H166" stroke="#9ebdcb" stroke-width="7" stroke-linecap="round"/>
      </g>
    `,
  },
  {
    id: '07',
    stem: '07-paper-owl',
    name: 'Paper owl',
    description: 'Folded blue paper forms a calm owl with ivory cheeks and a copper beak.',
    defs: `
      <linearGradient id="owl-blue" x1="0" y1="0" x2=".8" y2="1"><stop stop-color="#42b7d1"/><stop offset=".65" stop-color="#1c7da8"/><stop offset="1" stop-color="#185283"/></linearGradient>
      <linearGradient id="owl-ivory" x1="0" y1="0" x2=".4" y2="1"><stop stop-color="#ffffef"/><stop offset="1" stop-color="#dfdfc7"/></linearGradient>
      <linearGradient id="owl-face" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#fffef2"/><stop offset="1" stop-color="#f1ecda"/></linearGradient>
    `,
    art: `
      <path d="M20 22L80 54Q128 36 176 54L236 22L225 157Q216 208 128 240Q40 208 31 157Z" fill="#194b73"/>
      <path d="M20 18L80 50Q128 32 176 50L236 18L222 147Q215 199 128 233Q41 199 34 147Z" fill="url(#owl-blue)"/>
      <path d="M20 18L80 50L45 95Z" fill="#69d2e0"/>
      <path d="M236 18L176 50L211 95Z" fill="#40a1bb"/>
      <path d="M80 50L128 86L176 50L128 37Z" fill="#62c2d3"/>
      <path d="M45 95L80 70L128 103L176 70L211 95L205 149L178 182L128 207L78 182L51 149Z" fill="url(#owl-ivory)"/>
      <path d="M45 95L80 70L128 103L102 146L65 146Z" fill="url(#owl-face)"/>
      <path d="M211 95L176 70L128 103L154 146L191 146Z" fill="url(#owl-face)"/>
      <path d="M45 95L65 146L78 182L51 149Z" fill="#bed1cc"/>
      <path d="M211 95L191 146L178 182L205 149Z" fill="#a4c0bf"/>
      <path d="M78 182L128 157L128 207Z" fill="#f9f5e6"/>
      <path d="M178 182L128 157L128 207Z" fill="#dbdfc9"/>
      <ellipse cx="82" cy="119" rx="22" ry="26" fill="#e3c879"/>
      <ellipse cx="174" cy="119" rx="22" ry="26" fill="#e3c879"/>
      <ellipse cx="84" cy="117" rx="14" ry="20" fill="#143446"/>
      <ellipse cx="172" cy="117" rx="14" ry="20" fill="#143446"/>
      <ellipse cx="80" cy="108" rx="5" ry="6" fill="#fff"/>
      <ellipse cx="168" cy="108" rx="5" ry="6" fill="#fff"/>
      <path d="M110 143L128 131L146 143L128 171Z" fill="#e59537"/>
      <path d="M128 131L146 143L128 171Z" fill="#ad5c24"/>
      <path d="M34 147L78 182L128 233Q48 204 34 147Z" fill="#3f9fb5"/>
      <path d="M222 147L178 182L128 233Q208 204 222 147Z" fill="#126887"/>
    `,
  },
  {
    id: '08',
    stem: '08-kingfisher',
    name: 'Kingfisher',
    description: 'A folded kingfisher in turquoise and deep blue, with a bright orange breast.',
    defs: `
      <linearGradient id="bird-teal" x1="0" y1="0" x2=".8" y2="1"><stop stop-color="#51d5df"/><stop offset=".48" stop-color="#17a6bd"/><stop offset="1" stop-color="#147493"/></linearGradient>
      <linearGradient id="bird-wing" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#228fc9"/><stop offset="1" stop-color="#1a3f8a"/></linearGradient>
      <linearGradient id="bird-orange" x1="0" y1="0" x2=".9" y2="1"><stop stop-color="#ffbb58"/><stop offset="1" stop-color="#da6a27"/></linearGradient>
    `,
    art: `
      <path d="M100 167L78 238L40 243L66 181Z" fill="#18457a"/>
      <path d="M100 167L78 238L68 204L66 181Z" fill="#277eaf"/>
      <path d="M84 192L81 227L53 239Z" fill="#1e638e"/>
      <path d="M130 177L137 218L155 223" fill="none" stroke="#b7682e" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M151 176L159 216L178 220" fill="none" stroke="#db8d3f" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M95 41L124 20L158 25L181 48L178 73L166 89Q199 133 181 171Q167 199 132 202Q102 202 80 179L59 164L67 104Z" fill="url(#bird-teal)"/>
      <path d="M124 20L158 25L181 48L133 40L95 41Z" fill="#75e1e0"/>
      <path d="M159 54L183 48L242 76L177 77Z" fill="#e99a3e"/>
      <path d="M166 68L242 76L177 77Z" fill="#a95123"/>
      <path d="M166 88Q200 133 181 171Q167 199 132 202L121 178L138 106Z" fill="url(#bird-orange)"/>
      <path d="M177 75L167 91L157 105L136 113L134 90L151 77Z" fill="#f9f7df"/>
      <path d="M96 81L141 105L141 150L121 188L74 181L59 164L67 104Z" fill="url(#bird-wing)"/>
      <path d="M96 81L141 105L112 132L59 164L67 104Z" fill="#2badcb"/>
      <path d="M141 105L141 150L121 188L112 132Z" fill="#166aaf"/>
      <path d="M59 164L112 132L121 188L74 181Z" fill="#17538d"/>
      <path d="M97 82L131 86L141 105Z" fill="#116c9b"/>
      <path d="M136 49L158 51L173 65L145 72L125 65Z" fill="#145578"/>
      <circle cx="154" cy="58" r="8.5" fill="#0d2c3c"/>
      <circle cx="157" cy="55" r="2.6" fill="#fff"/>
      <path d="M181 171L157 186L132 202Q165 204 181 171Z" fill="#bf5724"/>
    `,
  },
];
