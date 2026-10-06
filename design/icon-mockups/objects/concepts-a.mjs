const gradient = (id, colors, vertical = false) => `<linearGradient id="${id}" x1="0" y1="0" x2="${vertical ? 0 : 1}" y2="1">${colors.map(([offset, color]) => `<stop offset="${offset}" stop-color="${color}"/>`).join('')}</linearGradient>`;

export default [
  {
    id: '01',
    stem: '01-marked-page',
    name: 'Marked page',
    description: 'An ivory sheet and a blue bookmark, familiar and easy to recognize.',
    defs: gradient('paper', [[0, '#fffefa'], [1, '#d9e3e6']])
      + gradient('ribbon', [[0, '#41b9ff'], [0.5, '#1685df'], [1, '#0957ac']])
      + gradient('fold', [[0, '#eef5f7'], [1, '#a9becd']]),
    art: `<path d="M52 22H165L216 72V225Q216 245 197 245H54Q39 245 39 229V39Q39 22 52 22Z" fill="#071b2d" opacity=".19"/>
      <path d="M48 13H163L210 60V220Q210 236 194 236H48Q33 236 33 220V29Q33 13 48 13Z" fill="url(#paper)" stroke="#b8cbd3" stroke-width="2"/>
      <path d="M163 15L163 57Q163 67 175 67H209V60Z" fill="#6a889b" opacity=".18"/>
      <path d="M162 13V47Q162 61 176 61H210Z" fill="url(#fold)"/>
      <path d="M40 31V216Q40 229 54 229" fill="none" stroke="#fff" stroke-opacity=".65" stroke-width="3"/>
      <path d="M70 15H120V148L95 131L70 148Z" fill="#0c3857" opacity=".16" transform="translate(4 4)"/>
      <path d="M69 13H120V143L94.5 124L69 143Z" fill="url(#ribbon)"/>
      <path d="M73 17H77V131L73 134Z" fill="#b6eaff" opacity=".55"/>
      <path d="M60 173H181M60 194H160" fill="none" stroke="#93aaba" stroke-width="9" stroke-linecap="round"/>
      <path d="M139 95H181M139 116H175" fill="none" stroke="#b4c5ce" stroke-width="8" stroke-linecap="round"/>`,
  },
  {
    id: '02',
    stem: '02-page-and-lens',
    name: 'Page and lens',
    description: 'A blue magnifying glass over a page, built around finding the details.',
    defs: gradient('paper', [[0, '#fffdf4'], [1, '#cad9df']])
      + gradient('ring', [[0, '#5cd1ff'], [0.4, '#178ed9'], [1, '#125199']])
      + gradient('glass', [[0, '#f4feff'], [0.5, '#c7e9f4'], [1, '#7cbdd8']])
      + gradient('brass', [[0, '#ffe6a0'], [0.4, '#e5b64e'], [1, '#a97124']]),
    art: `<path d="M38 18H122L166 60V194Q166 205 153 205H39Q26 205 26 191V32Q26 18 38 18Z" fill="#0a2336" opacity=".2" transform="translate(4 5)"/>
      <path d="M38 13H123L166 56V188Q166 202 152 202H38Q24 202 24 188V27Q24 13 38 13Z" fill="url(#paper)" stroke="#aabdca" stroke-width="2"/>
      <path d="M123 13V44Q123 56 135 56H166Z" fill="#b7cbd7"/>
      <path d="M48 74H101M48 99H96M48 124H84M48 150H75" stroke="#8ea9bc" stroke-width="9" stroke-linecap="round"/>
      <path d="M187 173L238 219Q245 228 237 236Q229 244 220 237L172 187Z" fill="#061f37" opacity=".25" transform="translate(1 3)"/>
      <path d="M185 166L239 218Q247 226 238 235Q230 244 221 235L171 183Z" fill="url(#brass)" stroke="#a47429" stroke-width="2"/>
      <path d="M186 176L233 224" stroke="#fff1ba" stroke-width="5" stroke-linecap="round" opacity=".75"/>
      <path d="M176 163L195 181L181 195L162 176Z" fill="#154f7d"/>
      <circle cx="143" cy="132" r="66" fill="#0c2a45" opacity=".18" transform="translate(2 5)"/>
      <circle cx="143" cy="130" r="64" fill="url(#ring)" stroke="#1569a6" stroke-width="2"/>
      <circle cx="143" cy="130" r="48" fill="url(#glass)" stroke="#0e5e96" stroke-width="3"/>
      <path d="M116 115H153M116 140H166" fill="none" stroke="#528daf" stroke-width="10" stroke-linecap="round" opacity=".7"/>
      <path d="M105 121Q110 97 134 93" fill="none" stroke="#fff" stroke-opacity=".83" stroke-width="7" stroke-linecap="round"/>
      <path d="M93 102A57 57 0 0 1 155 75" fill="none" stroke="#c7f1ff" stroke-width="3" stroke-linecap="round" opacity=".85"/>
      <path d="M158 173Q174 166 180 153" fill="none" stroke="#ebfbff" stroke-width="3" stroke-linecap="round" opacity=".45"/>`,
  },
  {
    id: '03',
    stem: '03-blueprint-roll',
    name: 'Blueprint roll',
    description: 'A rolled blue plan and a golden ruler for a more technical identity.',
    defs: gradient('plan', [[0, '#43b1e7'], [0.5, '#238ac8'], [1, '#1261a1']])
      + gradient('roll', [[0, '#8ddaf7'], [0.38, '#4eb5e5'], [1, '#14679e']])
      + gradient('ruler', [[0, '#ffe6a2'], [0.5, '#efc254'], [1, '#bd872a']])
      + gradient('curl', [[0, '#d5f2ff'], [0.42, '#91d0ec'], [1, '#479bc2']]),
    art: `<g transform="rotate(-8 126 125)">
      <path d="M43 28H188Q205 28 205 45V210H48Q26 210 26 191V49Q26 28 43 28Z" fill="#07273c" opacity=".2" transform="translate(4 5)"/>
      <path d="M43 24H185Q201 24 201 40V205H42Q25 205 25 188V44Q25 24 43 24Z" fill="url(#plan)" stroke="#196793" stroke-width="2"/>
      <path d="M53 27H182Q197 27 197 42" fill="none" stroke="#a1e0fa" stroke-opacity=".65" stroke-width="3"/>
      <path d="M53 44V172M79 46V169M105 46V169M131 46V169M157 46V169M183 46V169M52 65H185M52 91H185M52 117H185M52 143H185" fill="none" stroke="#b2e2f4" stroke-opacity=".18" stroke-width="2"/>
      <path d="M68 153V71H158V153H128V122H101V153Z" fill="none" stroke="#e2f7ff" stroke-width="7" stroke-linejoin="round"/>
      <path d="M112 73V99H155" fill="none" stroke="#e2f7ff" stroke-width="5"/>
      <path d="M43 24Q25 24 25 44V189Q25 172 43 172H55V41Q55 24 43 24Z" fill="url(#roll)"/>
      <path d="M35 44V166" fill="none" stroke="#d9f7ff" stroke-width="4" stroke-opacity=".65"/>
      <path d="M43 172H201V190Q201 208 184 208H43Q25 208 25 190Q25 172 43 172Z" fill="url(#curl)"/>
      <ellipse cx="43" cy="190" rx="18" ry="18" fill="#296f99"/>
      <ellipse cx="43" cy="190" rx="10" ry="11" fill="#104b78"/>
      <path d="M54 181H186" stroke="#ecfbff" stroke-width="4" stroke-linecap="round" opacity=".75"/>
      </g>
      <g transform="rotate(-34 164 162)">
        <rect x="143" y="79" width="44" height="164" rx="5" fill="#092235" opacity=".23" transform="translate(4 3)"/>
        <rect x="140" y="76" width="44" height="164" rx="5" fill="url(#ruler)" stroke="#ab7a2b" stroke-width="2"/>
        <path d="M144 84V232" fill="none" stroke="#fff2b9" stroke-width="3" opacity=".8"/>
        <path d="M172 94H183M165 114H183M172 134H183M165 154H183M172 174H183M165 194H183M172 214H183" fill="none" stroke="#916627" stroke-width="4"/>
        <circle cx="162" cy="228" r="4" fill="#9a6b28"/>
      </g>`,
  },
  {
    id: '04',
    stem: '04-drafting-compass',
    name: 'Drafting compass',
    description: 'A brass drawing compass on pale paper, precise and recognizable.',
    defs: gradient('paper', [[0, '#fffdf2'], [1, '#c9d8df']])
      + gradient('brass', [[0, '#fff1b8'], [0.45, '#dfb357'], [1, '#a77426']])
      + gradient('blue', [[0, '#59bef0'], [0.48, '#278acf'], [1, '#15598d']])
      + gradient('steel', [[0, '#f5faff'], [0.42, '#b9c9d2'], [1, '#607b8f']]),
    art: `<g transform="rotate(9 128 145)">
      <path d="M64 55H189Q200 55 200 66V222Q200 233 189 233H64Q53 233 53 222V66Q53 55 64 55Z" fill="#071e2d" opacity=".18" transform="translate(4 4)"/>
      <rect x="53" y="51" width="147" height="177" rx="11" fill="url(#paper)" stroke="#a9becb" stroke-width="2"/>
      <path d="M67 207A57 57 0 0 1 176 169" fill="none" stroke="#6aa8ca" stroke-width="4"/>
      <path d="M81 186H166M122 145V215" fill="none" stroke="#9cc0d1" stroke-width="3"/>
      </g>
      <path d="M129 51L39 206L31 239L57 216L143 66Z" fill="#072137" opacity=".18" transform="translate(4 3)"/>
      <path d="M145 51L215 204L223 236L201 221L123 65Z" fill="#072137" opacity=".18" transform="translate(4 3)"/>
      <path d="M119 54L36 204L55 216L143 67Z" fill="url(#brass)" stroke="#a9772d" stroke-width="2" stroke-linejoin="round"/>
      <path d="M120 66L44 203" fill="none" stroke="#fff3bd" stroke-width="5" stroke-linecap="round" opacity=".8"/>
      <path d="M36 204L29 237L54 215Z" fill="url(#steel)"/>
      <path d="M127 63L199 214L217 203L150 52Z" fill="url(#brass)" stroke="#a9772d" stroke-width="2" stroke-linejoin="round"/>
      <path d="M148 65L208 203" fill="none" stroke="#fff0b4" stroke-width="4" stroke-linecap="round" opacity=".8"/>
      <path d="M185 167L207 157L231 211Q234 218 226 222L219 225Q212 228 209 221Z" fill="url(#blue)" stroke="#145688" stroke-width="2"/>
      <path d="M196 169L217 216" fill="none" stroke="#9adfff" stroke-width="4" opacity=".7"/>
      <path d="M220 224L233 242L232 216Z" fill="url(#steel)"/>
      <path d="M97 113Q136 128 174 111" fill="none" stroke="#8a6835" stroke-width="11"/>
      <path d="M97 111Q136 126 174 109" fill="none" stroke="#e5c170" stroke-width="5"/>
      <rect x="125" y="11" width="21" height="33" rx="7" fill="url(#blue)" stroke="#175d92" stroke-width="2"/>
      <path d="M131 18V31" stroke="#b1e6ff" stroke-width="3" stroke-linecap="round" opacity=".75"/>
      <circle cx="135" cy="55" r="26" fill="url(#brass)" stroke="#a5772f" stroke-width="2"/>
      <circle cx="135" cy="55" r="13" fill="url(#blue)" stroke="#155c91" stroke-width="2"/>
      <path d="M130 50L140 60" stroke="#bde7f9" stroke-width="4" stroke-linecap="round"/>`,
  },
];
