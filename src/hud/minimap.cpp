/*
 * MODIFIED in 2026 by EveryNightLive (unofficial 32:9 update):
 *   32:9 support, compatibility with the Dec/2017 DDDA.exe and recent
 *   Special K versions, full map / menu / HUD fixes, build fixes.
 *   Changes are marked with "FIX" comments. See README.md.
 */

#include <stdint.h>
#include <Windows.h>
#include <cstring>

#include "minimap.h"
#include "../hook.h"
#include "../config.h"
#include "../log.h"
#include "../render.h"

ad_minimap_s* minimap = nullptr;

// FIX (2026): Deteccao do mapa grande sem endereco fixo.
//
//   O hook original usava o endereco fixo 0x0068BA30 (DDDA.exe de 2016). No
//   executavel atual (dez/2017) esse endereco cai no MEIO de uma instrucao e
//   o jogo crashava ao abrir o mapa.
//
//   Agora localizamos a classe uGUIMap em tempo de execucao pelo sistema de
//   tipos do MT Framework (MtDTI) e enganchamos a entrada 55 da sua vtable:
//   um metodo __thiscall, sem parametros, executado a cada quadro SOMENTE
//   enquanto o menu do mapa esta aberto. O hook so registra "o mapa esta
//   aberto"; quem decide se o desenho atual e o mapa grande e o codigo de
//   renderizacao, ao detectar o shader do minimapa (ver render.cpp).
//
//   __fastcall recebe ECX ("this") e EDX como os dois primeiros parametros e
//   os repassa intactos ao original (compativel com __thiscall sem args).
static const int AD_UGUIMAP_UPDATE_SLOT = 55;

typedef void (__fastcall *uGUIMap_update_pfn)(void* This, void* edx);
static uGUIMap_update_pfn uGUIMap_update_Original = nullptr;

static void
__fastcall
uGUIMap_update_Detour (void* This, void* edx)
{
  if (minimap != nullptr)
    minimap->map_menu_tick = GetTickCount ();

  uGUIMap_update_Original (This, edx);
}

bool
ad_minimap_s::isMapMenuActive (void)
{
  // O update roda no loop do jogo, o desenho logo depois; 250 ms de folga
  //   cobre quedas de FPS e o quadro em que o menu fecha.
  return map_menu_tick != 0 && (GetTickCount () - map_menu_tick) < 250;
}

//
// Busca de bytes simples dentro de um intervalo de memoria do proprio modulo
//
static uint8_t*
AD_FindBytes (uint8_t* begin, size_t len, const uint8_t* pat, size_t pat_len)
{
  if (len < pat_len)
    return nullptr;

  for (size_t i = 0; i <= len - pat_len; i++) {
    if (begin [i] == pat [0] && memcmp (begin + i, pat, pat_len) == 0)
      return begin + i;
  }

  return nullptr;
}

static bool
AD_GetSection (HMODULE mod, const char* name, uint8_t** start, size_t* size)
{
  PIMAGE_DOS_HEADER     dos = (PIMAGE_DOS_HEADER)mod;
  PIMAGE_NT_HEADERS     nt  = (PIMAGE_NT_HEADERS)((uint8_t *)mod + dos->e_lfanew);
  PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION (nt);

  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
    if (strncmp ((const char *)sec->Name, name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
      *start = (uint8_t *)mod + sec->VirtualAddress;
      *size  = sec->Misc.VirtualSize;
      return true;
    }
  }

  return false;
}

//
// Localiza a vtable de uGUIMap:
//   1. string "uGUIMap\0" em .rdata (nome registrado no MtDTI)
//   2. em .text: "push <string>" seguido de "mov ecx, <objeto MtDTI>"
//      (construtor estatico do MtDTI da classe)
//   3. em .text: "mov eax, <MtDTI>; ret" = metodo virtual getDTI()
//   4. em .rdata: ponteiro para getDTI() = entrada 4 da vtable
//
void**
AD_FindUGUIMapVTable (HMODULE exe)
{
  uint8_t *text,  *rdata;
  size_t   text_len, rdata_len;

  if (! (AD_GetSection (exe, ".text",  &text,  &text_len) &&
         AD_GetSection (exe, ".rdata", &rdata, &rdata_len))) {
    dll_log.Log (L" [Minimap] Could not read executable sections");
    return nullptr;
  }

  static const uint8_t name [] = "uGUIMap"; // inclui o '\0' final

  uint32_t dti = 0;

  for (size_t off = 1; off + sizeof (name) <= rdata_len && dti == 0; off++) {
    uint8_t* str = AD_FindBytes (rdata + off, rdata_len - off, name, sizeof (name));

    if (str == nullptr)
      break;

    off = (size_t)(str - rdata);

    if (str [-1] != 0) // precisa ser inicio de string
      continue;

    uint8_t push_str [5] = { 0x68 };
    uint32_t str_va      = (uint32_t)(uintptr_t)str;
    memcpy (&push_str [1], &str_va, 4);

    uint8_t* scan     = text;
    size_t   scan_len = text_len;

    while (dti == 0) {
      uint8_t* hit = AD_FindBytes (scan, scan_len, push_str, 5);

      if (hit == nullptr)
        break;

      for (int k = 5; k < 5 + 16 && (hit + k + 5) <= (text + text_len); k++) {
        if (hit [k] == 0xB9) { // mov ecx, imm32
          memcpy (&dti, &hit [k + 1], 4);
          break;
        }
      }

      scan_len -= (size_t)(hit + 1 - scan);
      scan      = hit + 1;
    }
  }

  if (dti == 0) {
    dll_log.Log (L" [Minimap] uGUIMap type descriptor (MtDTI) not found");
    return nullptr;
  }

  uint8_t getter_code [6] = { 0xB8, 0, 0, 0, 0, 0xC3 };
  memcpy (&getter_code [1], &dti, 4);

  uint8_t* getter = AD_FindBytes (text, text_len, getter_code, 6);

  if (getter == nullptr) {
    dll_log.Log (L" [Minimap] uGUIMap::getDTI not found (MtDTI=%08X)", dti);
    return nullptr;
  }

  uint32_t getter_va = (uint32_t)(uintptr_t)getter;

  for (size_t off = 0; off + 4 <= rdata_len; off += 4) {
    if (memcmp (rdata + off, &getter_va, 4) == 0) {
      void** vtable = (void **)(rdata + off) - 4; // getDTI e a entrada 4

      dll_log.Log ( L" [Minimap] uGUIMap: MtDTI=%08X getDTI=%08X vtable=%08X",
                      dti, getter_va, (uint32_t)(uintptr_t)vtable );
      return vtable;
    }
  }

  dll_log.Log (L" [Minimap] uGUIMap vtable not found (getDTI=%08X)", getter_va);
  return nullptr;
}

void
ad_minimap_s::notifyShaderChange ( uint32_t vs_crc32,
                                   uint32_t ps_crc32,
                                   bool     pixel )
{
  if (drawing && pixel) {
    ++shader_changes;
    if (! main_map) {
      if (shader_changes > 2) {
        finished = true;
        drawing  = false;
      }
    } else {
      if (ps_crc32 == 0xf88d8bcd) {
#if 0
        if (tracer.log_frame && config.trace.minimap) {
          dll_log.Log ( L" Ending map menu draw <%f,%f,%f> (vs: %x, ps: %x)",
                          minimap->prim_xpos,
                            minimap->prim_ypos,
                              minimap->prim_zpos,
                                vs_checksum,
                                  ps_checksum );
          }
#endif
        main_map = false;
        finished = true;
        drawing  = false;
      }
    }
  }
}

//
// FIX (2026): "Altura de tela" vista pelo codigo do mapa grande.
//
//   O uGUIMap le a altura da tela (campo +0xBC de um objeto global) em 8
//   pontos e calcula, por exemplo, (altura - largura * 9/16) / 2 para
//   encaixar um layout 16:9. Em 32:9 o mapa fica com metade da altura
//   relativa ao resto da UI. Fazemos essas 8 leituras (e SO elas) lerem esta
//   variavel, que vale largura * 9/16 no MapMode=1 - assim o mapa se comporta
//   como numa tela 16:9, igual ao resto da interface, e preenche a area
//   central inteira com cursor e icones alinhados.
//
//   Enderecos do DDDA.exe de dez/2017; cada instrucao e conferida byte a
//   byte antes de ser alterada (se o jogo mudar, nada e alterado).
//
volatile int ad_map_screen_height = 0; // 0 = ainda nao calculado
static   bool ad_map_height_patched = false;

// Com o patch ativo o proprio mapa ja se encaixa na faixa central 16:9;
//   o mod nao deve reduzir a largura do viewport de novo (ver render.cpp).
bool
AD_IsMapHeightPatchActive (void)
{
  return ad_map_height_patched &&
         ad_map_screen_height > 0 &&
         ad_map_screen_height != (int)ad::RenderFix::height;
}

void
AD_UpdateMapScreenHeight (void)
{
  int w = (int)ad::RenderFix::width;
  int h = (int)ad::RenderFix::height;

  if (w <= 0 || h <= 0)
    return;

  int fake = h;

  if (config.render.map_height_patch && config.render.map_mode == 1 &&
      (float)w / (float)h > (16.0f / 9.0f) + 0.01f)
    fake = (w * 9) / 16;

  ad_map_screen_height = fake;
}

struct ad_map_height_site_s {
  uint32_t addr;
  uint8_t  orig [8];
  int      len;     // 6 = mov r32,[r+0BCh] / 8 = movd xmm,[r+0BCh]
  bool     patch;   // false = so conferido (leitura ligada ao cursor)
};

// As 3 leituras marcadas "false" convertem a posicao do cursor entre a tela
//   e o mapa (e o retangulo de "cursor dentro do mapa"). Elas precisam da
//   altura REAL, a mesma do sistema de entrada do jogo; com a altura falsa o
//   cursor ficava deslocado e nao alcancava a borda inferior (sem rolagem).
static const ad_map_height_site_s ad_map_height_sites [] = {
  { 0x00681597, { 0x8B, 0x80, 0xBC, 0x00, 0x00, 0x00             }, 6, true  },
  { 0x0068230B, { 0x8B, 0x80, 0xBC, 0x00, 0x00, 0x00             }, 6, true  },
  { 0x00686FA3, { 0x66, 0x0F, 0x6E, 0x80, 0xBC, 0x00, 0x00, 0x00 }, 8, false }, // cursor x janela
  { 0x0068769D, { 0x8B, 0x80, 0xBC, 0x00, 0x00, 0x00             }, 6, true  },
  { 0x0068EA4F, { 0x8B, 0x89, 0xBC, 0x00, 0x00, 0x00             }, 6, true  },
  { 0x0068F30B, { 0x8B, 0x92, 0xBC, 0x00, 0x00, 0x00             }, 6, true  },
  { 0x006927A1, { 0x66, 0x0F, 0x6E, 0x81, 0xBC, 0x00, 0x00, 0x00 }, 8, false }, // mapa -> tela (cursor)
  { 0x006928FE, { 0x8B, 0x92, 0xBC, 0x00, 0x00, 0x00             }, 6, false }, // tela -> mapa (cursor)
};

static void
AD_PatchMapScreenHeight (void)
{
  if (! config.render.map_height_patch) {
    dll_log.Log (L" [Minimap] Map height patch DISABLED by INI (MapHeightPatch=false)");
    return;
  }

  const int count = sizeof (ad_map_height_sites) / sizeof (ad_map_height_sites [0]);

  // Confere TODOS antes de alterar qualquer um
  for (int i = 0; i < count; i++) {
    const ad_map_height_site_s& s = ad_map_height_sites [i];

    if (memcmp ((void *)(uintptr_t)s.addr, s.orig, s.len) != 0) {
      dll_log.Log ( L" [Minimap] Map height patch: unexpected code at %08X; "
                    L"game version differs, NOT patching.", s.addr );
      return;
    }
  }

  // Valor inicial seguro ate o swapchain ser capturado: a altura real
  if (ad_map_screen_height == 0) {
    uint32_t* pScreen = *(uint32_t **)(uintptr_t)0x018D08F4;

    if (pScreen != nullptr)
      ad_map_screen_height = *(int *)((uint8_t *)pScreen + 0xBC);

    if (ad_map_screen_height <= 0)
      ad_map_screen_height = 720;
  }

  uint32_t var_addr = (uint32_t)(uintptr_t)&ad_map_screen_height;

  int patched = 0;

  for (int i = 0; i < count; i++) {
    const ad_map_height_site_s& s = ad_map_height_sites [i];

    if (! s.patch)
      continue;

    ++patched;

    uint8_t  code [8];
    memcpy (code, s.orig, s.len);

    int modrm = (s.len == 6) ? 1 : 3;
    code [modrm] = (uint8_t)((code [modrm] & 0x38) | 0x05); // [disp32]
    memcpy (&code [modrm + 1], &var_addr, 4);

    DWORD dwOld;
    VirtualProtect ((void *)(uintptr_t)s.addr, s.len, PAGE_EXECUTE_READWRITE, &dwOld);
    memcpy ((void *)(uintptr_t)s.addr, code, s.len);
    VirtualProtect ((void *)(uintptr_t)s.addr, s.len, dwOld, &dwOld);
  }

  FlushInstructionCache (GetCurrentProcess (), nullptr, 0);

  ad_map_height_patched = true;

  dll_log.Log ( L" [Minimap] Map height patch applied (%d of %d sites, variable at %08X)",
                  patched, count, var_addr );
}

void
ad_minimap_s::init (void)
{
  AD_PatchMapScreenHeight ();

  // INI: [AgDrag.Render] HookMap=false desliga a deteccao do mapa.
  if (! config.render.hook_map) {
    dll_log.Log (L" [Minimap] Map detection hook DISABLED by INI (HookMap=false)");
    return;
  }

  void** vtable = AD_FindUGUIMapVTable (GetModuleHandle (nullptr));

  if (vtable == nullptr) {
    dll_log.Log (L" [Minimap] Map detection unavailable; the full map will not be corrected.");
    return;
  }

  uint8_t* target = (uint8_t *)vtable [AD_UGUIMAP_UPDATE_SLOT];

  // Sanidade: o alvo precisa estar no codigo do executavel e comecar com
  //   um prologo normal (push ebp; mov ebp, esp). Se o jogo mudar, falhamos
  //   com seguranca em vez de corromper codigo.
  HMODULE  exe = GetModuleHandle (nullptr);
  uint8_t* text;
  size_t   text_len;

  if ( (! AD_GetSection (exe, ".text", &text, &text_len)) ||
       target < text || target >= text + text_len         ||
       target [0] != 0x55 || target [1] != 0x8B || target [2] != 0xEC ) {
    dll_log.Log ( L" [Minimap] vtable[%d] = %08X does not look like the expected function; not hooking.",
                    AD_UGUIMAP_UPDATE_SLOT, (uint32_t)(uintptr_t)target );
    return;
  }

  MH_STATUS st =
    AD_CreateFuncHook ( L"uGUIMap::update (vtable[55])", target,
                          uGUIMap_update_Detour,
                (LPVOID *)&uGUIMap_update_Original );

  dll_log.Log ( L" [Minimap] uGUIMap vtable[%d] = %08X, hook create status: %d",
                  AD_UGUIMAP_UPDATE_SLOT, (uint32_t)(uintptr_t)target, (int)st );

  if (st == MH_OK)
    AD_EnableHook (target);
}

void
ad_minimap_s::reset (void)
{
  drawing        = false;
  finished       = false;
  main_map       = false;
  shader_changes = 0;
  prims_drawn    = 0;
  prim_xpos      = 0.0f;
  prim_ypos      = 0.0f;
  prim_zpos      = 0.0f;
  ps23           = 0.0f;
  ps43           = 0.0f;
  center_prim    = false;
}