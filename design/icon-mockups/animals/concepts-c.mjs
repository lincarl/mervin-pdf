// Four animal studies with broad paper folds and freestanding silhouettes.
export default [
  {
    id: '09',
    stem: '09-tortoise',
    name: 'Tortoise',
    description: 'A domed teal shell with broad green folds and a calm profile.',
    defs: `
      <linearGradient id="tortoise-shell" x2=".7" y2="1"><stop stop-color="#75d4ad"/><stop offset=".55" stop-color="#319e92"/><stop offset="1" stop-color="#156776"/></linearGradient>
      <linearGradient id="tortoise-skin" x2=".8" y2="1"><stop stop-color="#c3dda0"/><stop offset="1" stop-color="#5ea47b"/></linearGradient>
      <linearGradient id="tortoise-center" x2=".7" y2="1"><stop stop-color="#91dfba"/><stop offset="1" stop-color="#3bac9b"/></linearGradient>
    `,
    art: `
      <path d="M53 149L17 170L50 177L72 166Z" fill="#4f9479"/>
      <path d="M83 174L88 209H116L121 177ZM132 175L140 207H164L169 176Z" fill="#3f7f6b"/>
      <path d="M69 163L49 187L43 217H79L96 178Z" fill="url(#tortoise-skin)"/>
      <path d="M166 163L156 186L164 217H200L192 184L185 164Z" fill="url(#tortoise-skin)"/>
      <path d="M49 187L67 187L60 217H43Z" fill="#77b28b"/>
      <path d="M164 190L182 184L200 217H176Z" fill="#5d9c79"/>
      <path d="M181 123L208 111L229 108L245 123L244 145L231 159L209 158L189 171L174 151Z" fill="url(#tortoise-skin)"/>
      <path d="M208 111L229 108L245 123L218 127L190 145L181 123Z" fill="#d2e6b6"/>
      <path d="M210 143L231 159L209 158L189 171L190 145Z" fill="#5b9473"/>
      <path d="M41 160L45 119L62 82L95 59L137 53L173 66L198 101L208 151L183 178L129 192L76 182Z" fill="url(#tortoise-shell)"/>
      <path d="M45 119L62 82L95 59L93 88L75 122Z" fill="#95ddba"/>
      <path d="M95 59L137 53L173 66L143 82L93 88Z" fill="#b4e8c8"/>
      <path d="M173 66L198 101L169 120L143 82Z" fill="#59baa3"/>
      <path d="M93 88L143 82L169 120L142 157L96 156L75 122Z" fill="url(#tortoise-center)"/>
      <path d="M143 82L169 120L142 157L128 122Z" fill="#31978f"/>
      <path d="M45 119L75 122L96 156L76 182L41 160Z" fill="#379b93"/>
      <path d="M96 156L142 157L129 192L76 182Z" fill="#247e83"/>
      <path d="M198 101L208 151L183 178L142 157L169 120Z" fill="#1c797f"/>
      <path d="M142 157L183 178L129 192Z" fill="#155e70"/>
      <circle cx="232" cy="127" r="6.5" fill="#21483e"/>
      <circle cx="234" cy="125" r="2" fill="#fffdef"/>
    `,
  },
  {
    id: '10',
    stem: '10-chameleon',
    name: 'Chameleon',
    description: 'A bright folded chameleon with a curled tail and a raised crest.',
    defs: `
      <linearGradient id="chameleon-body" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#b5df71"/><stop offset=".48" stop-color="#56b886"/><stop offset="1" stop-color="#168b8c"/></linearGradient>
      <linearGradient id="chameleon-tail" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#1da497"/><stop offset="1" stop-color="#21657f"/></linearGradient>
      <linearGradient id="chameleon-head" x2=".7" y2="1"><stop stop-color="#d3e986"/><stop offset="1" stop-color="#61b67c"/></linearGradient>
    `,
    art: `
      <path d="M105 143C80 132 56 133 39 148C19 165 16 191 31 208C46 226 74 231 88 214C98 202 92 185 78 182C69 180 61 185 60 193" fill="none" stroke="url(#chameleon-tail)" stroke-width="21" stroke-linecap="round"/>
      <path d="M111 145L97 176L105 189L130 191L124 180H114L127 153Z" fill="#367e72"/>
      <path d="M164 135L182 170L184 182L216 179L204 171L195 172L183 138Z" fill="#377f6b"/>
      <path d="M80 113L96 79L127 63L161 73L184 97L188 137L165 162L126 172L96 160L77 142Z" fill="url(#chameleon-body)"/>
      <path d="M96 79L127 63L161 73L139 103L80 113Z" fill="#b0de82"/>
      <path d="M80 113L139 103L126 172L96 160L77 142Z" fill="#65c19a"/>
      <path d="M139 103L184 97L188 137L165 162L126 172Z" fill="#279c8c"/>
      <path d="M160 83L175 58L187 31L209 74L240 96L227 125L197 139L172 130L157 109Z" fill="url(#chameleon-head)"/>
      <path d="M175 58L187 31L209 74L183 80Z" fill="#b7df76"/>
      <path d="M183 80L209 74L240 96L214 108Z" fill="#d6e9a0"/>
      <path d="M172 130L183 80L214 108L197 139Z" fill="#7dbe7e"/>
      <path d="M214 108L240 96L227 125L197 139Z" fill="#459b76"/>
      <path d="M220 114L235 109L227 125L215 128Z" fill="#387b65"/>
      <circle cx="205" cy="95" r="15" fill="#ebedab"/>
      <circle cx="209" cy="96" r="7" fill="#244b43"/>
      <circle cx="211" cy="93" r="2.2" fill="#fffdf0"/>
      <path d="M111 143L98 172L87 192L109 195L118 191L105 184L128 155Z" fill="#8bce98"/>
      <path d="M163 139L174 163L193 190L216 187L222 180L203 180L188 151Z" fill="#9bd298"/>
      <path d="M111 143L128 155L105 184L98 172Z" fill="#70ba8c"/>
    `,
  },
  {
    id: '11',
    stem: '11-manta-ray',
    name: 'Manta ray',
    description: 'Wide blue paper wings and a sturdy tail form a graceful manta.',
    defs: `
      <linearGradient id="manta-blue" x1="0" y1="0" x2=".8" y2="1"><stop stop-color="#64d6ed"/><stop offset=".48" stop-color="#238fcb"/><stop offset="1" stop-color="#244c94"/></linearGradient>
      <linearGradient id="manta-center" x2=".4" y2="1"><stop stop-color="#abe8ed"/><stop offset="1" stop-color="#3b9dca"/></linearGradient>
      <linearGradient id="manta-tail" x2="1" y2="1"><stop stop-color="#245da1"/><stop offset="1" stop-color="#173b73"/></linearGradient>
    `,
    art: `
      <path d="M115 157L135 160L137 201L148 233L137 242L125 223L117 193Z" fill="url(#manta-tail)"/>
      <path d="M104 76L100 53Q99 43 108 43L121 68ZM152 76L156 53Q157 43 148 43L135 68Z" fill="#4dbdd8"/>
      <path d="M104 73Q105 60 117 59H139Q151 60 152 73L158 87Q201 76 243 65Q217 119 173 145L146 173L128 181L110 173L83 145Q39 119 13 65Q55 76 98 87Z" fill="url(#manta-blue)"/>
      <path d="M13 65L98 87L128 116L83 145Q39 119 13 65Z" fill="#56c4e3"/>
      <path d="M13 65L83 145L63 116Z" fill="#93e0eb"/>
      <path d="M243 65L158 87L128 116L173 145Q217 119 243 65Z" fill="#2b85be"/>
      <path d="M243 65L173 145L202 108Z" fill="#4aabd4"/>
      <path d="M98 87L104 73Q128 60 152 73L158 87L144 136L128 163L112 136Z" fill="url(#manta-center)"/>
      <path d="M128 71L152 73L158 87L144 136L128 163Z" fill="#62bdd8"/>
      <path d="M83 145L112 136L128 163L110 173Z" fill="#216ab1"/>
      <path d="M173 145L144 136L128 163L146 173L128 181Z" fill="#204d93"/>
      <path d="M110 91L128 103L146 91L139 115H117Z" fill="#d2f1ed"/>
      <ellipse cx="106" cy="90" rx="5.5" ry="4.5" fill="#1a567d"/>
      <ellipse cx="150" cy="90" rx="5.5" ry="4.5" fill="#1a567d"/>
    `,
  },
  {
    id: '12',
    stem: '12-octopus',
    name: 'Octopus',
    description: 'An indigo and teal octopus with a folded head and six visible arms.',
    defs: `
      <linearGradient id="octopus-head" x1=".1" y1="0" x2=".85" y2="1"><stop stop-color="#9387e4"/><stop offset=".5" stop-color="#566bc4"/><stop offset="1" stop-color="#188f9e"/></linearGradient>
      <linearGradient id="octopus-left" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#8470d4"/><stop offset="1" stop-color="#3d69a7"/></linearGradient>
      <linearGradient id="octopus-right" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#3b81be"/><stop offset="1" stop-color="#159fa5"/></linearGradient>
    `,
    art: `
      <path d="M94 128C74 153 74 176 47 176C26 176 17 157 24 143Q29 134 37 135" fill="none" stroke="url(#octopus-left)" stroke-width="24" stroke-linecap="round"/>
      <path d="M162 128C182 153 182 176 209 176C230 176 239 157 232 143Q227 134 219 135" fill="none" stroke="url(#octopus-right)" stroke-width="24" stroke-linecap="round"/>
      <path d="M107 146C106 180 86 205 52 215Q37 219 31 207" fill="none" stroke="url(#octopus-left)" stroke-width="25" stroke-linecap="round"/>
      <path d="M149 146C150 180 170 205 204 215Q219 219 225 207" fill="none" stroke="url(#octopus-right)" stroke-width="25" stroke-linecap="round"/>
      <path d="M118 158C124 188 119 217 105 232" fill="none" stroke="#4569ac" stroke-width="25" stroke-linecap="round"/>
      <path d="M138 158C132 188 137 217 151 232" fill="none" stroke="#2488a6" stroke-width="25" stroke-linecap="round"/>
      <path d="M66 111L72 62L96 32L128 21L160 32L184 62L190 111L178 143L151 168H105L78 143Z" fill="url(#octopus-head)"/>
      <path d="M72 62L96 32L128 21L128 82L94 105L66 111Z" fill="#9489dc"/>
      <path d="M128 21L160 32L184 62L156 104L128 82Z" fill="#7383cf"/>
      <path d="M184 62L190 111L178 143L151 168L156 104Z" fill="#318fae"/>
      <path d="M66 111L94 105L105 168L78 143Z" fill="#6563b7"/>
      <path d="M94 105L128 82L156 104L151 168H105Z" fill="#6193c3"/>
      <path d="M128 82L156 104L151 168H128Z" fill="#498fb3"/>
      <ellipse cx="104" cy="119" rx="16" ry="19" fill="#f7f7e6"/>
      <ellipse cx="152" cy="119" rx="16" ry="19" fill="#f7f7e6"/>
      <ellipse cx="107" cy="121" rx="8" ry="11" fill="#183f59"/>
      <ellipse cx="149" cy="121" rx="8" ry="11" fill="#183f59"/>
      <circle cx="105" cy="117" r="3" fill="#fff"/>
      <circle cx="147" cy="117" r="3" fill="#fff"/>
    `,
  },
];
