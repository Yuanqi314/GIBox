#pragma once
#include <cstdint>
namespace ms::abi {
inline constexpr const char* buildId="2b4dd73155e845a2ef698ec500509baa4eb3bfa8";
// RVAs valid only for the above GNU Build ID.
enum Offset:uintptr_t {
 tick=0x7c7ecb8,manager=0x159c1638,
 getMesh=0x8d9dc9c,setMesh=0x8d9de7c,getBones=0x8d9dbd4,getMaterials=0x8dc2ff4,setMaterials=0x8dc3050,
 clone=0x8bf6420,destroy=0x8bf5ca0,getName=0x8bf5d94,getParent=0x8bf7518,
 arrayNew=0x43252b8,objectNew=0xfa5e86c,pin=0xcb0c140,unpin=0xcb0c158,
 clear=0x8bb769c,setIndexFormat=0x8bb7538,getBindposes=0x8bb7694,
 getPositions=0x8bb7adc,setPositions=0x8bb7aec,getNormals=0x8bb7afc,setNormals=0x8bb7b0c,
 getUV=0x8bb7b38,setUV=0x8bb7b48,setUV2=0x8bb7b68,setUV3=0x8bb7b88,setUV4=0x8bb7ba8,
 getColors=0x8bb7bd4,setColors=0x8bb7be8,getTangents=0x8bb7b1c,setTangents=0x8bb7b28,
 getTriangles=0x8bb7f08,setTriangles=0x8bb7fac,setSubmeshCount=0x8bb76ec,setSubmeshTriangles=0x8bb84b4,
 meshFinish=0x8bb8c28,meshUpload=0x8bb8e70,
 textureType=0x8fef0f4,textureInit=0x8fef020,texturePixels=0x8fef48c,textureApply=0x8fef5b0,
 mainTexture=0xc2c53c0,
 // newString(null, const char*) returns a managed System.String*; the two Material calls take that pointer (a property NAME), not an int ID.
 newString=0x8d93dcc,hasProperty=0xc2c5ebc,setTexture=0xc2c69b0,
 // Engine allocator: alloc(*(B+nativeAllocator), 0, bytes, 16, label 15, 0, name, 1); the skin-weight block of a built mesh.
 nativeAlloc=0x50c6270,nativeAllocator=0x15f3ef10,
 // Object.FindObjectsOfType(Type): managed entry is "MOV W1,#1 (active scene objects); B 0x5a982c0".
 // The native body reads [Type+0x10] (Il2CppType*), strings "FindAllObjectsOfType: ...". Returns Object[].
 findObjectsOfType=0x8bf5d8c,
 // The native body itself: (Type, mode). The object flag word (+0xC) filters by mode: 1 skips persistent objects (bit 20,
 // assets) and inactive ones; 2 skips nothing (the "MOV W1,#2" thunk at 0x49ea170, Resources.FindObjectsOfTypeAll).
 // Used by 0.3.4's template scan; unused since 0.3.5 (the async hand-off below replaced it).
 findObjectsNative=0x5a982c0,
 // GameObject.GetComponentsInChildren(Type, bool includeInactive) -> Component[]; Component.get_gameObject.
 componentsInChildren=0x875c09c,gameObject=0xc2c4744,
 // JDIIHHFJKBM.AAKKKEMMMCA(this, ulong key, bool success, uint handle, GameObject asset, MethodInfo*): the game's async
 // model hand-off. In handoff-20377 (7 hand-offs, 5 characters, party screen / detail page) the asset arrived as an
 // inactive GameObject whose SkinnedMeshRenderers already had Mesh, materials and bones, on the Tick thread, and nothing
 // changed them until after the entity's ready/activate calls.
 receive=0x88770e0,
 // The game's own mesh + bone binding (locator capture, PID 25113): both call the engine's internal mesh setter
 // (0x56d82b0, not the managed set_sharedMesh) and then bind bones from the mesh's bone-name hashes (native Mesh +0x90 is
 // their count). This is how the game put its mesh back over ours (hand-off, LOD changes).
 //   lodBind(loader native, Mesh native, renderer native)                    - native LOD switch
 //   meshBones(renderer native, Mesh native, hash container, bone container) - SkinnedMeshRenderer.BindMeshBones
 lodBind=0x5798010,meshBones=0x56e4d80,
 // Scripting::ScriptingWrapperFor(Object*) -> managed object (Texture2D.whiteTexture tail-calls it).
 wrapperFor=0x5a91f64,
 // il2cpp Reflection::GetTypeObject(const Il2CppType*) -> System.Type; IL2CPP typeof code calls it right before 0x8bf5d8c.
 typeObject=0x43066f4,
 // il2cpp Class::FromIl2CppType(const Il2CppType*, bool) -> Il2CppClass*; reads the type enum at +0xA.
 classFromType=0x4307304
};
// First 16 code bytes of the hand-off and binding entries (from the handoff and locator captures);
// the hand-off hook is installed only when all match.
struct CodeCheck {Offset rva;const char* hex;};
inline constexpr CodeCheck handoffChecks[]={{componentsInChildren,"e403022ae2031f2a23008052e5031f2a"},
 {gameObject,"614d1c16604d1c16c00000b4080840f9"},{receive,"fe4fbfa9288706f0e50304aae403032a"}};
inline constexpr CodeCheck bindChecks[]={{lodBind,"ff8304d1e9230b6dfd7b0ca9fc6f0da9"},{meshBones,"ff8304d1e85b00fdfd7b0ca9fc6f0da9"}};
}
