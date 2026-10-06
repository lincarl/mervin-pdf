// Standalone object studies. Every shape stays inside the transparent canvas.
export default [
  {
    id: '09',
    stem: '09-paper-glider',
    name: 'Paper glider',
    description: 'A folded ivory glider with teal folds and an orange underside.',
    defs: `
      <linearGradient id="glider-paper" x1="0" y1="0" x2="1" y2="1">
        <stop stop-color="#ffffff"/><stop offset=".55" stop-color="#f6faf6"/><stop offset="1" stop-color="#acd7db"/>
      </linearGradient>
      <linearGradient id="glider-wing" x1="0" y1="0" x2=".8" y2="1">
        <stop stop-color="#ffffff"/><stop offset="1" stop-color="#c7e7e7"/>
      </linearGradient>
      <linearGradient id="glider-fold" x2="1" y2="1">
        <stop stop-color="#2eb1bb"/><stop offset="1" stop-color="#076476"/>
      </linearGradient>
      <linearGradient id="glider-orange" x2="1" y2="1">
        <stop stop-color="#ffd077"/><stop offset="1" stop-color="#e87725"/>
      </linearGradient>`,
    art: `
      <path d="M20 105L237 22L172 232L123 161L70 199L89 131Z" fill="#183c4e" opacity=".18" transform="translate(0 4)"/>
      <path d="M20 101L237 18L172 228L123 157L70 195L89 127Z" fill="#eaf6f3" stroke="#537f8c" stroke-width="2.5" stroke-linejoin="round"/>
      <path d="M70 195L89 127L123 157Z" fill="url(#glider-orange)"/>
      <path d="M89 127L237 18L123 157L70 195Z" fill="url(#glider-fold)"/>
      <path d="M70 195L89 127L105 143Z" fill="url(#glider-orange)"/>
      <path d="M20 101L237 18L89 127Z" fill="url(#glider-wing)" stroke="#8eb6bc" stroke-width="1.5" stroke-linejoin="round"/>
      <path d="M237 18L172 228L123 157Z" fill="url(#glider-paper)" stroke="#83adb5" stroke-width="1.5" stroke-linejoin="round"/>
      <path d="M230 27L127 158L170 218" fill="none" stroke="#ffffff" stroke-opacity=".8" stroke-width="3" stroke-linejoin="round"/>
      <path d="M29 101L88 121L221 31" fill="none" stroke="#ffffff" stroke-opacity=".8" stroke-width="3" stroke-linecap="round"/>`,
  },
  {
    id: '10',
    stem: '10-focus-lens',
    name: 'Focus lens',
    description: 'A blue inspection lens enlarges a folded ivory document.',
    defs: `
      <linearGradient id="focus-paper" x2=".8" y2="1">
        <stop stop-color="#fffef2"/><stop offset="1" stop-color="#e2ddc6"/>
      </linearGradient>
      <linearGradient id="focus-rim" x2=".85" y2="1">
        <stop stop-color="#49b8e8"/><stop offset=".38" stop-color="#127bb9"/><stop offset="1" stop-color="#123e71"/>
      </linearGradient>
      <linearGradient id="focus-glass" x1=".12" y1="0" x2=".85" y2="1">
        <stop stop-color="#dcffff"/><stop offset=".55" stop-color="#8de3ed"/><stop offset="1" stop-color="#238cb9"/>
      </linearGradient>
      <linearGradient id="focus-glint" x2=".7" y2="1">
        <stop stop-color="#ffffff" stop-opacity=".8"/><stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
      </linearGradient>
      <clipPath id="focus-window"><circle cx="147" cy="145" r="73"/></clipPath>`,
    art: `
      <path d="M44 20H130L173 62V193Q173 209 157 209H44Q29 209 29 193V36Q29 20 44 20Z" fill="#173f54" opacity=".2" transform="translate(2 5)"/>
      <path d="M44 17H129L173 61V192Q173 208 157 208H44Q29 208 29 192V33Q29 17 44 17Z" fill="url(#focus-paper)" stroke="#a6ac9a" stroke-width="2.5"/>
      <path d="M129 17V49Q129 61 141 61H173Z" fill="#d1cfb4"/>
      <path d="M49 79H111M49 96H99" fill="none" stroke="#99aaa5" stroke-width="7" stroke-linecap="round"/>
      <circle cx="148" cy="149" r="94" fill="#12354c" opacity=".2"/>
      <circle cx="147" cy="145" r="92" fill="url(#focus-rim)" stroke="#0e527e" stroke-width="2.5"/>
      <circle cx="147" cy="145" r="78" fill="#063858"/>
      <circle cx="147" cy="145" r="73" fill="url(#focus-glass)"/>
      <g clip-path="url(#focus-window)">
        <path d="M103 85H150L180 115V204Q180 214 170 214H103Q93 214 93 204V95Q93 85 103 85Z" fill="#184d66" opacity=".16" transform="translate(4 5)"/>
        <path d="M103 81H150L180 111V201Q180 211 170 211H103Q93 211 93 201V91Q93 81 103 81Z" fill="#fffef0" stroke="#7baeb0" stroke-width="2"/>
        <path d="M150 81V102Q150 111 159 111H180Z" fill="#b9d0c7"/>
        <path d="M111 131H162M111 149H158M111 167H150" fill="none" stroke="#79a7ac" stroke-width="7" stroke-linecap="round"/>
        <path d="M62 105Q118 60 201 100L86 190Z" fill="url(#focus-glint)" opacity=".65"/>
      </g>
      <path d="M73 120A79 79 0 0 1 170 69" fill="none" stroke="#b6f3ff" stroke-width="4" stroke-linecap="round" opacity=".88"/>
      <path d="M224 157A79 79 0 0 1 161 223" fill="none" stroke="#073e69" stroke-width="4" stroke-linecap="round" opacity=".55"/>`,
  },
  {
    id: '11',
    stem: '11-set-square',
    name: 'Set square',
    description: 'An orange drafting triangle crossed by a blue pencil.',
    defs: `
      <linearGradient id="draft-gold" x1="0" y1="0" x2="1" y2="1">
        <stop stop-color="#ffe690"/><stop offset=".45" stop-color="#f7bc3d"/><stop offset="1" stop-color="#dd7c17"/>
      </linearGradient>
      <linearGradient id="draft-pencil" x1="0" y1="0" x2="1" y2=".6">
        <stop stop-color="#6bddf4"/><stop offset=".42" stop-color="#168ad1"/><stop offset="1" stop-color="#174c98"/>
      </linearGradient>
      <linearGradient id="draft-wood" x2="1" y2="1">
        <stop stop-color="#fff0bd"/><stop offset="1" stop-color="#d99c4b"/>
      </linearGradient>
      <linearGradient id="draft-cap" x2="1" y2="1">
        <stop stop-color="#6cc4e8"/><stop offset="1" stop-color="#16457f"/>
      </linearGradient>`,
    art: `
      <path d="M29 29V225H233ZM69 118L138 187H69Z" fill="#593a18" fill-rule="evenodd" opacity=".2" transform="translate(1 5)"/>
      <path d="M29 25V221H233ZM69 114L138 183H69Z" fill="url(#draft-gold)" fill-rule="evenodd" stroke="#a5641a" stroke-width="2.5" stroke-linejoin="round"/>
      <path d="M37 44V212H211" fill="none" stroke="#ffecaa" stroke-width="4" stroke-linejoin="round"/>
      <path d="M69 114V183H138" fill="none" stroke="#a5641a" stroke-width="4" stroke-linejoin="round"/>
      <path d="M77 201V218M105 205V218M133 201V218M161 205V218M189 201V218M33 79H45M33 107H41M33 135H45M33 163H41M33 191H45" fill="none" stroke="#9c651d" stroke-width="3"/>
      <path d="M47 232L75 179L194 43L214 61L95 197Z" fill="#1b3546" opacity=".2" transform="translate(3 3)"/>
      <path d="M46 229L74 176L95 194Z" fill="url(#draft-wood)" stroke="#986e3c" stroke-width="1.5" stroke-linejoin="round"/>
      <path d="M46 229L58 205L65 213Z" fill="#283942"/>
      <path d="M74 176L192 41L213 60L95 194Z" fill="url(#draft-pencil)" stroke="#174d82" stroke-width="2" stroke-linejoin="round"/>
      <path d="M74 176L192 41L198 47L81 182Z" fill="#8be8f4" opacity=".75"/>
      <path d="M89 188L207 54L213 60L95 194Z" fill="#104a8c" opacity=".85"/>
      <path d="M192 41L206 25Q211 19 217 24L229 35Q234 40 229 46L213 64Z" fill="url(#draft-cap)" stroke="#234f7c" stroke-width="2"/>
      <path d="M188 45L209 64L217 55L196 36Z" fill="#d0e6ee"/>
      <path d="M191 47L211 65" fill="none" stroke="#8aaabc" stroke-width="2"/>`,
  },
  {
    id: '12',
    stem: '12-page-and-pen',
    name: 'Page and pen',
    description: 'An ivory page with a blue fountain pen and a brass nib.',
    defs: `
      <linearGradient id="pen-paper" x1="0" y1="0" x2=".75" y2="1">
        <stop stop-color="#fffef5"/><stop offset="1" stop-color="#e8e2ca"/>
      </linearGradient>
      <linearGradient id="pen-barrel" x1="0" y1="0" x2="1" y2="0">
        <stop stop-color="#174f88"/><stop offset=".28" stop-color="#32a6d5"/><stop offset=".5" stop-color="#197db8"/><stop offset="1" stop-color="#123d73"/>
      </linearGradient>
      <linearGradient id="pen-brass" x1="0" y1="0" x2="1" y2=".5">
        <stop stop-color="#a86f25"/><stop offset=".35" stop-color="#ffe5a0"/><stop offset=".58" stop-color="#edc367"/><stop offset="1" stop-color="#aa702c"/>
      </linearGradient>`,
    art: `
      <path d="M42 17H142L190 65V222Q190 238 174 238H42Q26 238 26 222V33Q26 17 42 17Z" fill="#273f48" opacity=".17" transform="translate(3 5)"/>
      <path d="M42 14H142L190 62V220Q190 236 174 236H42Q26 236 26 220V30Q26 14 42 14Z" fill="url(#pen-paper)" stroke="#a9aa98" stroke-width="2.5"/>
      <path d="M142 14V49Q142 62 155 62H190Z" fill="#d2d0b8"/>
      <path d="M142 16V49Q142 61 155 61H186" fill="none" stroke="#f8f5df" stroke-width="3"/>
      <path d="M51 83H137M51 106H121M51 129H100" fill="none" stroke="#9baea9" stroke-width="7" stroke-linecap="round"/>
      <path d="M48 202Q55 188 65 194Q72 199 80 190" fill="none" stroke="#287797" stroke-width="5" stroke-linecap="round"/>
      <g transform="translate(143 135) rotate(36)">
        <path d="M-19-80Q-19-103 0-103Q19-103 19-80V30L23 39L16 65L0 107L-16 65L-23 39L-19 30Z" fill="#153a51" opacity=".19" transform="translate(4 3)"/>
        <path d="M-19-80Q-19-103 0-103Q19-103 19-80V27H-19Z" fill="url(#pen-barrel)" stroke="#113d67" stroke-width="2"/>
        <path d="M-10-77V10" fill="none" stroke="#72d6ee" stroke-width="4" stroke-linecap="round" opacity=".78"/>
        <path d="M8-82V-39Q8-29 14-29" fill="none" stroke="#f1d28b" stroke-width="5" stroke-linecap="round"/>
        <path d="M-22 30H22L23 42L15 68L0 107L-15 68L-23 42Z" fill="url(#pen-brass)" stroke="#9c6e2f" stroke-width="2" stroke-linejoin="round"/>
        <path d="M-20 18H20V33H-20Z" fill="url(#pen-brass)" stroke="#a87c3b" stroke-width="1.5"/>
        <path d="M0 59V103" fill="none" stroke="#785024" stroke-width="2.5"/>
        <circle cy="58" r="5" fill="#6c522d"/>
        <path d="M-13 42L-9 64L-2 85" fill="none" stroke="#fff1b8" stroke-width="3" stroke-linecap="round"/>
      </g>`,
  },
];
