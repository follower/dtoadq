#define MAX_DEPTH 5
#define MARCH_DIST //%MARCH_DIST.0f
#define MARCH_REPS //%MARCH_REPS
#define MAT_LEN //%MAT_LENGTH

#define TEXTURE_T __read_only image2d_array_t
#define SCENE_T(S, T) SceneInfo* S, TEXTURE_T T
// EMIT_MAT = -120 - light_index
#define EMIT_MAT -120
#define REMIT(I) REmission(EMIT_MAT - I, si->debug_values, si->time)

#define DFLT2(V) ((V).x), ((V).y)
#define DFLT3(V) ((V).x), ((V).y), ((V).z)
#define DFLT4(V) ((V).x), ((V).y), ((V).z), ((V).w)

#define writeln(X)     if (Is_Debug()){printf(X "\n");                         }
#define writeint(X)    if (Is_Debug()){printf(#X " %d\n",                 (X));}
#define writeint2(X)   if (Is_Debug()){printf(#X " %d, %d\n",             (X));}
#define writeptr(X)    if (Is_Debug()){printf(#X " %p\n",                 (X));}
#define writefloat(X)  if (Is_Debug()){printf(#X " %f\n",                 (X));}
#define writefloat2(X) if (Is_Debug()){printf(#X " %f, %f\n",        DFLT2(X));}
#define writefloat3(X) if (Is_Debug()){printf(#X " %f, %f, %f\n",    DFLT3(X));}
#define writefloat4(X) if (Is_Debug()){printf(#X " %f, %f, %f, %f\n",DFLT4(X));}

// math macros
#define SQR(T) ((T)*(T))

//
#define Spectrum float3

__constant float MARCH_ACC = //%MARCH_ACC.0f/1000.0f;
__constant int   DO_NAVIGATION = 1;
__constant int   M_diffuse      = 1, M_glossy       = 2,
                 M_specular     = 3, M_transmittive = 4;
// -----------------------------------------------------------------------------
// --------------- DEBUG -------------------------------------------------------
// Variadic functions not supported, so this is best you get :-(
bool Is_Debug ( ) {
  return get_global_id(0) == get_global_size(0)/2 &&
         get_global_id(1) == get_global_size(1)/2;
}

// -----------------------------------------------------------------------------
// --------------- GPU-CPU STRUCTS ---------------------------------------------
typedef struct T_Camera {
  float3 position, lookat, up;
  int2 dim;
  float fov;
  int flags;
} Camera;

typedef struct T_Material {
  // colour [set to (-1.0, -1.0, -1.0) to have map override it]
  float3 albedo;
  // sampling strategy
  float diffuse, specular, glossy, glossy_lobe;
  float transmittive;
  // PBR material
  float roughness, metallic, fresnel, subsurface, anisotropic;
} Material;

typedef struct T_Emitter {
  float3 origin, emission;
  float radius;
} Emitter;

typedef struct T_SharedInfo {
  unsigned char clear_img;
  unsigned long finished_samples;
  unsigned char spp;
  uint2 rng_state;
} SharedInfo;

__constant float PI   = 3.141592653589793f;
__constant float IPI  = 0.318309886183791f;
__constant float IPI2 = 0.159154943091895f;
__constant float TAU  = 6.283185307179586f;
__constant float ITAU = 0.159154943091895f;
// -----------------------------------------------------------------------------
// --------------- GENERAL STRUCTS ---------------------------------------------
typedef struct T_Ray {
  float3 origin, dir;
} Ray;

typedef struct T_SampledPt {
  float3 colour, origin, dir, normal;
  float dist;
  int mat_index;
} SampledPt;
SampledPt SampledPt_From_Origin(float3 origin) {
  SampledPt pt;
  pt.origin = origin;
  pt.dist = 0.0f;
  return pt;
}

typedef struct T_Vertex {
  float3 origin, albedo, normal;
  float pdf_fwd, pdf_bwd;
  Material* material;
  int M_ID;
} Vertex;

float Calc_Prob(Vertex* t) {
  return t->pdf_bwd/t->pdf_fwd;
}

typedef struct T_Subpath {
  Vertex vertices[MAX_DEPTH];
  uint length;
} Subpath;

typedef struct T_SceneInfo {
  float time;
  // __read_only image2d_array_t textures; IMAGES CANT BE USED AS FIELD TYPES:-(
  Material* materials;
  float3 debug_values;
  uint2 rng_state;
} SceneInfo;
SceneInfo New_SceneInfo(float time, Material* materials,
                        float3 debug_values, uint2 rng_state) {
  SceneInfo si;
  si.time         = time;
  si.materials    = materials;
  si.debug_values = debug_values;
  si.rng_state    = rng_state;
  return si;
}
// -----------------------------------------------------------------------------
// --------------- RANDOM FUNCTIONS       --------------------------------------
/*
  Using a high quality uniform RNG is very important for monte carlo, thus I
    use the MWC64X.
    http://cas.ee.ic.ac.uk/people/dt10/research/rngs-gpu-mwc64x.html

  The Warp Geneartor is, while superior, does not allow divergence, which makes
  it practically useless. For example, can't really know how much random numbers
  you'll need for a path you haven't generated yet of unknown length.
*/

float Rand ( SceneInfo* si ) {
  enum { A=4294883355U };
  uint2 r = (*si).rng_state;
  uint res = r.x^r.y;
  uint hi = mul_hi(r.x, A);
  r.x = r.x*A + r.y;
  r.y = hi + (r.x<r.y);
  (*si).rng_state = r;
  return res/(float)(UINT_MAX);
}
// -----------------------------------------------------------------------------
// --------------- GENERAL FUNCTIONS      --------------------------------------

float Sample_Uniform ( SceneInfo* si ) {
  return Rand(si);
}
float2 Sample_Uniform2 ( SceneInfo* si ) {
  return (float2)(Sample_Uniform(si), Sample_Uniform(si));
}
float3 Sample_Uniform3 ( SceneInfo* si ) {
  return (float3)(Sample_Uniform(si), Sample_Uniform2(si));
}

float sqr(float t) { return t*t; }
float Distance(float3 u, float3 v) {
  float x = u.x-v.x, y = u.y-v.y, z = u.z-v.z;
  return sqrt(x*x + y*y + z*z);
}
float Distance_Sqr(float3 u, float3 v) {
  float x = u.x-v.x, y = u.y-v.y, z = u.z-v.z;
  return (x*x + y*y + z*z);
}

float Power_Heuristic ( float fn, float fpdf, float gn, float gpdf ) {
  float f = sqr(fn*fpdf),
        g = sqr(gn*gpdf);
  return f/(f + g);
}
// -----------------------------------------------------------------------------
// --------------- MAP GEOMETRY FUNCTIONS --------------------------------------

//---MAP GEOMETRY INSERTION POINT---
//%MAPFUNCDECLARATIONS
//----------------------------------
//%MAPFUNCDEFINITIONS
//----------------------------------

void MapUnionG( int avoid, SampledPt* d1, float d, int mi, float3 c ) {
  if ( mi != avoid && d1->dist > d ) {
    d1->colour = c;
    d1->dist = d;
    d1->mat_index = mi;
  }
}

// -----------------------------------------------------------------------------
// --------------- SCENE -------------------------------------------------------
//%SCENEINSERT
//------------------------
// -----------------------------------------------------------------------------
// --------------- MAP ---------------------------------------------------------
SampledPt Map ( int a, float3 origin, SCENE_T(si, Tx))  {
  SampledPt res;
  res.dist = FLT_MAX;

  //---MAP INSERTION POINT---
  //%MAPINSERT
  //-------------------------

  // lighting with emissions
  float3 light_emission = (float3)(1.0f, 0.9f, 0.8f);
  for ( int i = 0; i != EMITTER_AMT; ++ i ) {
    Emitter e = REmission(i, si->debug_values, si->time);
    float dist = sdSphere(origin - e.origin, e.radius);
    MapUnionG(a, &res, dist, EMIT_MAT - i, light_emission*e.emission);
  }

  return res;
}

// -----------------------------------------------------------------------------
// --------------- RAYMARCHING      --------------------------------------------
SampledPt March ( int avoid, Ray ray, SCENE_T(si, Tx)) {
  float distance = 0.001f;
  SampledPt t_info;
  for ( int i = 0; i < MARCH_REPS; ++ i ) {
    t_info = Map(avoid, ray.origin + ray.dir*distance, si, Tx);
    if ( t_info.dist < MARCH_ACC || t_info.dist > MARCH_DIST ) break;
    distance += t_info.dist;
    if ( t_info.mat_index != avoid ) avoid = -1;
  }
  t_info.dir    = ray.dir;
  if ( t_info.dist > MARCH_DIST ) {
    t_info.dist = -1.0f;
    return t_info;
  }
  t_info.dist = distance;
  t_info.origin = ray.origin + ray.dir*t_info.dist;
  return t_info;
}

// -----------------------------------------------------------------------------
// --------------- GRAPHIC FUNCS -----------------------------------------------
float3 Normal ( float3 p, SCENE_T(s, t)) {
  float2 e = (float2)(1.0f, -1.0f)*0.5773f*0.0005f;
  return normalize(
    e.xyy*Map(-1, p + e.xyy, s, t).dist + e.yyx*Map(-1, p + e.yyx, s, t).dist +
    e.yxy*Map(-1, p + e.yxy, s, t).dist + e.xxx*Map(-1, p + e.xxx, s, t).dist);
}

float3 RColour ( float3 pt_colour, Material* m ) {
  return (m->albedo.x < 0.0f) ? pt_colour : m->albedo;
}

float3 reflect ( float3 V, float3 N ) {
  return V - 2.0f*dot(V, N)*N;
}

float3 refract(float3 V, float3 N, float refraction) {
  float cosI = -dot(N, V);
  float cosT = 1.0f - refraction*refraction*(1.0f - cosI*cosI);
  return (refraction*V) + (refraction*cosI - sqrt(cosT))*N;
}

float3 To_Cartesian ( float cos_theta, float phi ) {
  float sin_theta = sqrt(fmax(0.0f, 1.0f - cos_theta));
  return (float3)(cos(phi)*sin_theta, sin(phi)*sin_theta, cos_theta);
}

float3 Binormal ( float3 N ) {
  float3 axis = (fabs(N.x) < 1.0f ? (float3)(1.0f, 0.0f, 0.0f) :
                                    (float3)(0.0f, 1.0f, 0.0f));
  return normalize(cross(N, axis));
}

// from PBRT 3rd ed
float2 Concentric_Sample_Disk(SceneInfo* si) {
  float2 u = Sample_Uniform2(si);
  // maps to [-1, 1]^2
  float2 offset = 2.0f * u - (float2)(1.0f, 1.0f);
  if ( offset.x == 0.0f && offset.y == 0.0f )
    return (float2)(0.0f);

  float theta, r;
  if ( fabs(offset.x) > fabs(offset.y) ) {
    r = offset.x;
    theta = (PI/4.0f) * (offset.y/offset.x);
  } else {
    r = offset.y;
    theta = (PI/2.0f) * (offset.x/offset.y);
  }
  return r * (float2)(cos(theta), sin(theta));
}

// from PBRT 3rd ed
float3 Sample_Cosine_Hemisphere ( SceneInfo* si, float* pdf ) {
  const float2 d = Concentric_Sample_Disk(si);
  float phi = sqrt(fmax(0.0f, 1.0f - d.x*d.x - d.y*d.y));
  *pdf = phi * IPI;
  return normalize((float3)(d.x, d.y, phi));
}
float PDF_Cosine_Hemisphere ( float3 wi, float3 N ) {
  return dot(wi, N) * IPI;
}

// from Embree renderer (though, unlike PBRT's rather unique method of
// calculating a cosine hemisphere, I've seen this method in many places)
float3 Sample_Cosine_Sphere ( SceneInfo* si, float* pdf ) {
  float2 u = Sample_Uniform2(si);
  float phi = TAU * u.x,
        vv  = 2.0f*(u.y - 0.5f),
        cos_theta = sign(vv) * sqrt(fabs(vv));
  *pdf = 2.0f * cos_theta * IPI;
  return To_Cartesian(sign(vv) * sqrt(fabs(vv)), phi);
}
float PDF_Cosine_Sphere ( float3 wi, float3 N ) {
  return 2.0f * fabs(dot(wi, N)) * IPI;
}

float _PDF_Uniform_Cone ( float cos_theta_max ) {
  return 1.0f/(4.0f*PI*sqr(sin(0.5f*cos_theta_max)));
}
float3 Sample_Uniform_Cone ( float cos_theta_max, float* pdf, SceneInfo* si ) {
  float2 u = Sample_Uniform2(si);
  float phi = TAU*u.x,
        cos_theta = 1.0f - u.y*(1.0f - cos(cos_theta_max));
  *pdf = _PDF_Uniform_Cone(cos_theta_max);
  return To_Cartesian(cos_theta, phi);
}
float PDF_Uniform_Cone ( float3 wi, float3 N, float cos_theta_max ) {
  return 1.0f/(4.0f*PI*sqr(sin(0.5f*cos_theta_max)));
}

float3 Reorient_Angle ( float3 wi, float3 N ) {
  float3 binormal  = Binormal(N),
         bitangent = cross(binormal, N);
  return bitangent*wi.x + binormal*wi.y + wi.z*N;
}

// -----------------------------------------------------------------------------
// --------------- BSDF    FUNCS -----------------------------------------------
// --- some brdf utility functions .. ---
float Schlick_Fresnel ( float u ) {
  float f = clamp(1.0f - u, 0.0f, 1.0f);
  float f2 = f*f;
  return f2*f2*f; // f^5
}

float Smith_G_GGX_Correlated ( float L, float R, float a ) {
  return L * sqrt(R - a*sqr(R) + a);
}

// -------- samples
float3 BRDF_Diffuse_Sample ( float3 wi, float3 N, float* pdf, SceneInfo* si ) {
  return Reorient_Angle(Sample_Cosine_Hemisphere(si, pdf), N);
}
float BRDF_Diffuse_PDF  ( float3 wi, float3 N ) {
  return PDF_Cosine_Hemisphere(wi, N);
}
float3 BRDF_Glossy_Sample ( float3 wi, float3 N, float* pdf, float glossy_lobe,
                            SceneInfo* si) {
  return Reorient_Angle(Sample_Uniform_Cone(glossy_lobe, pdf, si), N);
}
float BRDF_Glossy_PDF   ( float3 wi, float3 N, float cos_theta_max ) {
  return PDF_Uniform_Cone(wi, N, cos_theta_max);
}
float3 BRDF_Specular_Sample ( float3 wi, float3 N, float* pdf ) {
  *pdf = 0.0f;
  return reflect(wi, N);
}
float BRDF_Specular_PDF ( float3 wi, float3 N ) {
  return 0.0f;
}


float3 BRDF_Transmittive_Sample ( float3 wi, float3 N, float* pdf, float ior ) {
  *pdf = 0.0f;
  return refract(wi, N, ior);
}


// Actual BRDF function that returns the albedo of the surface
float3 _BRDF_F ( float3 wi, float3 N, float3 wo, Material* m ) {
  // get binormal, bitangent, half vec etc
  const float3 binormal  = Binormal(N),
               bitangent = cross(binormal, N),
               L         =  wo, V = -wi,
               H         = normalize(L+V);
  const float  cos_NV    = dot(N, V), cos_NL     = dot(N, L),
               cos_HV    = dot(H, V), cos_HL     = dot(H, L),
               Fresnel_L = Schlick_Fresnel(cos_NL),
               Fresnel_V = Schlick_Fresnel(cos_NV);
  // Diffusive component, just made it up
  float3 diffusive_albedo = m->albedo * pow(cos_HL * cos_NV, 0.5f) * IPI;

  float3 microfacet = (float3)(1.0f);

  if ( cos_NL < 0.0f || cos_NV < 0.0f )
    return (float3)(0.0f);

  { // ------- Fresnel
    // modified diffusive fresnel from disney, modified to use albedo & F0
    const float F0 = m->fresnel * m->metallic,
                Fresnel_diffuse_90 = F0 * SQR(cos_HL);
    microfacet *= (1.0f - F0) * diffusive_albedo +
                    mix(1.0f, Fresnel_diffuse_90, Fresnel_L) *
                    mix(1.0f, Fresnel_diffuse_90, Fresnel_V);
  }
  { // ------- Geometric
    // Heits 2014, SmithGGXCorrelated with half vec combined with anisotropic
    // term using disney's GTR2_aniso model
    const float Param  = sqr(0.5f + sqr(m->roughness)),
                Aspect = sqrt(1.0f - m->anisotropic*0.9f),
                Ax     = Param/Aspect, Ay = Param*Aspect,
                GGX_NV = Smith_G_GGX_Correlated(cos_HL, cos_NV, Ax),
                GGX_HL = Smith_G_GGX_Correlated(cos_NV, cos_HL, Ay);
    microfacet *= 0.5f / (GGX_NV*Ax + GGX_HL*Ay);
  }
  { // ------- Distribution
    // Hyper-Cauchy Distribution using roughness and metallic
    const float Param = 1.0f + m->roughness,
                Shape = (1.1f - sqrt(m->metallic)),
                tan_HL = length(cross(H, L))/cos_HL;
    const float Upper  = (Param - 1.0f)*pow(sqrt(2.0f), (2.0f*Param - 2.0f)),
                LowerL = (PI*sqr(Shape) * pow(cos_HL, 4.0f)),
                LowerR = pow(2.0f + sqr(tan_HL)/sqr(Shape), Param);
    microfacet *= (Upper / (LowerL * LowerR));
  }

  // Since microfacet is described using half vec, the following energy
  // conservation model may be used [Edwards et al. 2006]
  microfacet /= 4.0f * cos_HV * fmax(cos_NL, cos_NV);

  { // --------- Subsurface
    // modified disney retro reflection based off Hanrahan-Grueger BSSRDF
    //   approximation
    const float Rr_term = (2.0f * (0.5f + m->roughness) * sqr(dot(N, H)));
    const float3 Retro_reflection = diffusive_albedo * Rr_term *
      (Fresnel_L + Fresnel_V + (Fresnel_L*Fresnel_V*(Rr_term - 1.0f)));
    diffusive_albedo = mix(diffusive_albedo, Retro_reflection, m->subsurface);
  }

  return (microfacet + diffusive_albedo);
}

// BSDF Sample helper function that sets sample dir, pdf and f
void _BSDF_Sample ( float3 wi, float3 N, Material* m,
                    float3* sample_dir, float* sample_pdf, float3* sample_f,
                    SceneInfo* si ) {
  // get sample_dir and pdf which is based off material brdf samples
  // diffuse -> glossy -> specular
  float remainder = 1.0f - m->diffuse;
  float rem_t = Sample_Uniform(si);
  int finished = 0;

  // diffuse
  if ( remainder <= rem_t ) {
    *sample_dir = BRDF_Diffuse_Sample(wi, N, sample_pdf, si);
    finished = 1;
  }
  // glossy
  remainder -= m->glossy;
  if ( !finished && remainder <= rem_t ) {
    *sample_dir = BRDF_Glossy_Sample(wi, N, sample_pdf, m->glossy_lobe, si);
    finished = 1;
  }
  // specular
  if ( !finished ) {
    *sample_dir = BRDF_Specular_Sample(wi, N, sample_pdf);
  }

  // get sample_f
  *sample_f = _BRDF_F(wi, N, *sample_dir, m);
}

// generates PDF, wo doesn't really do anything
float BSDF_PDF ( float3 sample_dir, float3 wo, Vertex* vtx ) {
  float pdf = 0.0f;
  Material* m = vtx->material;
  float3 N = vtx->normal;

  pdf += m->diffuse  * BRDF_Diffuse_PDF  (sample_dir, N);
  pdf += m->glossy   * BRDF_Glossy_PDF   (sample_dir, N, m->glossy_lobe);
  pdf += m->specular * BRDF_Specular_PDF (sample_dir, N);

  writefloat(m->glossy);

  return pdf;
}

float3 BSDF_Connect ( Vertex* V0, Vertex* V1, Vertex* V2 ) {
  float3 wi = V1->origin - V0->origin,
         wo = V2->origin - V1->origin;
  float3 ret = (float3)(0.0f);
  float weight = 0.0f;

  return _BRDF_F(wi, V1->normal, wo, V1->material);
}

/* sets vtx with random sample from material
*/
void BSDF_Sample ( float3 O, float3 wi, float3 N, Material* m,
                   Ray* ray, float3* albedo, float* pdf,
                   Vertex* prev_vtx, Vertex* vtx, SceneInfo* si){
  // bsdf info
  float3 sample_dir, sample_f;
  float sample_pdf;
  _BSDF_Sample(wi, N, m, &sample_dir, &sample_pdf, &sample_f, si);
  // edge info
  float3 edge = O - prev_vtx->origin;
  float edge_dist = length(edge);
  edge /= edge_dist; // normalize
  // vertex info
  vtx->origin   = O;
  vtx->normal   = N;
  vtx->pdf_fwd  = (*pdf) * dot(edge, prev_vtx->normal)/(SQR(edge_dist));
  vtx->material = m;
  vtx->albedo = vtx->albedo;
  // prev vertex pdf bwd
  prev_vtx->pdf_bwd = BSDF_PDF(sample_dir, wi, vtx) *
                       fabs(dot(-edge, N))/SQR(edge_dist);
  // misc scope info
  *albedo = sample_f * (*albedo) * fabs(dot(N, sample_dir))/sample_pdf;
  *ray    = (Ray){O, sample_dir};
  *pdf    = sample_pdf;
}


// -----------------------------------------------------------------------------
// --------------- LIGHT   FUNCS -----------------------------------------------
float Visibility_Ray(float3 orig, float3 other, SCENE_T(si, Tx)) {
  float theoretical = Distance(orig, other);
  float3 dir = normalize(other - orig);
  orig += dir*(0.001f);
  SampledPt ptinfo = March(-1, (Ray){orig, dir}, si, Tx);
  float actual = ptinfo.dist + MARCH_ACC + 0.1f;
  return 1.0f*(actual >= theoretical);
}

// -----------------------------------------------------------------------------
// --------------- LIGHT TRANSPORT ---------------------------------------------

/// Geometric term of e0 -> e1 w/ visibilty check, can be used to convert
///   a PDF w.r.t. SA to area by multiplying it by this factor
// ω -> A = PDF * |cosθ|/d²
// Adapted from PBRT 3d edition pg 1011
float Geometric_Term(Vertex* V0, Vertex* V1, SCENE_T(si, Tx)) {
  float3 edge = V1->origin - V0->origin;
  float dist = length(edge);
  edge /= dist; // normalize

  float visible = Visibility_Ray(V0->origin, V1->origin, si, Tx);
  float cos_wi = fmax(0.0f, dot( edge, V0->normal)),
        cos_wo = fmax(0.0f, dot(-edge, V1->normal));
  return cos_wi * cos_wo / SQR(dist) * visible;
}

__constant int Eval_Vertex_Enum_Hit   = 0,
               Eval_Vertex_Enum_Miss  = 1,
               Eval_Vertex_Enum_Light = 2;
int Eval_Vertex ( int* mindex, Ray* ray, float3* albedo, float* pdf,
                  Vertex* V0, Vertex* V1, SCENE_T(si, Tx) ) {
  // russian roulette
  if ( Sample_Uniform(si) < 0.05f ) return Eval_Vertex_Enum_Miss;
  // raymarch
  SampledPt ptinfo = March(*mindex, *ray, si, Tx);
  *mindex = ptinfo.mat_index;
  if ( *mindex <= EMIT_MAT ) return Eval_Vertex_Enum_Light;
  if ( ptinfo.dist < 0.0f || *mindex < 0 ) return Eval_Vertex_Enum_Miss;
  // grab info
  float3 O = ray->origin + ray->dir * ptinfo.dist,
         N = Normal(O, si, Tx);
  Material* mat = si->materials + ptinfo.mat_index;
  // sample
  BSDF_Sample(O, ray->dir, N, mat, ray, albedo, pdf, V0, V1, si);
  // no point in continuing anymore if energy lost
  // though it might be wise to continue anyway to avoid branching
  /* if ( albedo->x == 0.0f && albedo->y == 0.0f && albedo->z == 0.0f ) */
  /*   return Eval_Vertex_Enum_Miss; */
  return Eval_Vertex_Enum_Hit;
}

Emitter Generate_Light_Subpath ( Subpath* path, float* light_mis,
                                 float* light_prob, SCENE_T(si, Tx)) {
  // Grab a random light source to generate a subpath
  int light_index = Sample_Uniform(si)*EMITTER_AMT;
  Emitter light = REmission(light_index, si->debug_values, si->time);
  int mindex = EMIT_MAT - light_index;
  Ray ray;
  float3 albedo;
  float pdf_fwd;
  {// Generate ray position/angle, set initial vertex and set pdf_fwd
    float pdf_pos, pdf_dir;
    float3 N = Sample_Cosine_Sphere(si, &pdf_pos);
    float3 origin = light.origin + light.radius*N;
    float3 dir = Reorient_Angle(Sample_Cosine_Hemisphere(si, &pdf_dir), N);

    ray = (Ray){origin, dir};
    pdf_fwd = pdf_dir;

    Vertex* pv = path->vertices;
    pv->origin = origin;
    albedo = pv->albedo = light.emission/pdf_pos;
    pv->normal = N;
    pv->pdf_fwd = pdf_pos;
    pv->material = NULL;
    path->length = 1;
  }

  float t_prob = 1.0f, t_mis = 0.0f;

  // Generate path
  for ( int depth = 1; depth != 5; ++ depth ) {
    Vertex* L0 = path->vertices+depth-1,
          * L1 = path->vertices+depth;
    if ( Eval_Vertex(&mindex, &ray, &albedo, &pdf_fwd, L0, L1, si, Tx) )
      break;
    // weights for L0 now that its bdf pwd is defined
    t_prob = light_prob[depth-1] = t_prob * (L0->pdf_bwd/L0->pdf_fwd);
    t_mis  = light_prob[depth-1] = t_mis  + SQR(t_prob);
    path->length += 1;
  }
  return light;
}

Spectrum BDPT_Integrate ( float3 pixel, float3 dir, SCENE_T(si, Tx)) {
  Subpath path;
  float light_mis_arr[MAX_DEPTH];
  float light_prob_arr[MAX_DEPTH];
  // α₀⁽ᴸᴱ⁾ = 1
  // instead of generating a light and bsdf path, only the light path is
  // generated and the BSDF path is walked while evaluating the vertex behind it
  // to conserve GPU memory.
  Emitter light = Generate_Light_Subpath(&path, light_mis_arr,
                                         light_prob_arr, si, Tx);
  Spectrum sample_colour = (Spectrum)(-1.0f, -1.0f, -1.0f);
  // --- generate eye path ---
  Ray ray = (Ray){pixel, dir};
  Vertex E0, E1, E2;
  float3 albedo = (float3)(1.0f);
  float pdf_fwd = 1.0f;
  { // Generate initial vertex and pdf_fwd
    E2.origin = ray.origin;
    E2.albedo = (float3)(1.0f);
    E2.normal = ray.dir;
    E2.pdf_fwd = 1.0f;
    E2.material = NULL;
  }

  /* float2 path_con[MAX_DEPTH+MAX_DEPTH]; */

  int mindex = -1;
  // Generate path, in non-gpu bdpt pseudo/D-code
  /* Connect ( eye_verts[2..$] , light_verts[1..$] ) */
  /* One immediate problem is that, since the eye path is connected during
    generation, and the next vertex needs to be known, the results are stored in
    E0, E1. E2 is the current evaluation vertex.
    where &#R = ready to render

       Eval Shift Eval Shift Eval Rendr       Shift  Eval   Rendr
   E0  NUL  NUL   NUL  &0R   &0R  &0R   . . . &N-2R  &N-1R  &N-1R
   E1  NUL  &0    &0R  &1    &1R  &1R         &N-1   &N-1R  &N-1R
   E2  &0   NUL   &1   NUL   &2   &2          NUL    &N     &N
  */
  float t_prob = 1.0f, t_mis = 0.0f;
  for ( int eye_depth = 1; eye_depth != 6; ++ eye_depth ) {
    // shift edges
    E0 = E1;
    E1 = E2;
    {// eval path
      int result = Eval_Vertex(&mindex, &ray, &albedo, &pdf_fwd,
                               &E1, &E2, si, Tx);
      if ( result == Eval_Vertex_Enum_Miss )
        break;
      if ( result == Eval_Vertex_Enum_Light )
        return (Spectrum)(1.0f);
    }
    if ( eye_depth == 1 ) continue;
    // TODO if eye delta continue
    /* Perform connection, in pseudo/D-code: */
    /*    light_path.Each!( L => L.Connect(E1) ) */
    for ( int light_depth = 0; light_depth < path.length; ++ light_depth ) {
      Vertex* L1 = (path.vertices + light_depth),
            * L0 = light_depth == 0 ? NULL : (path.vertices + light_depth-1);
      // if delta continue

      Spectrum contribution = E1.albedo * L1->albedo;

      if ( light_depth > 0 ) { // s >= 1, t >= 1 strategy
        // Connect E0 -> E1 <-> L1 -> L0
        contribution = BSDF_Connect(&E0, &E1,  L1) *
                       BSDF_Connect( L0,  L1, &E1);
      } else { // s == 1 strategy. L1 = L0; L1 'undefined'
        // Connect E0 -> E1 -> L1
        // have to calculate the geometric term for 'L0->L1'
        float gterm = fabs(dot(L1->normal, -normalize(L1->origin - E1.origin)));
        contribution = BSDF_Connect(&E0, &E1, L1) * gterm;
      }

      // Geometric connection term [includes visibility check]
      contribution *= Geometric_Term(&E1, L1, si, Tx);


      // ------------ Breaking the connectioning ------------
      // Create temporary as their bwd pdfs are overwriten in connection
      // strategy
      // Not necessary for TE1 but still done for consistency
      Vertex TL1, TL0, TE0, TE1;
      TE0 = E0; TE1 = E1;
      TL1 = *L1;
      if ( L0 ) TL0 = *L0;

      // E1 pdf bwd
      // No S=0 strat as all direct hits on light are thrown out
      if        ( light_depth == 0 ) {// S=1 strat
        float3 edge = TE1.origin - TL1.origin;
        float edge_dist = length(edge);
        edge /= edge_dist; // normalize

        TE1.pdf_bwd = fmax(0.0f, dot(TL1.normal, edge)) *
                      dot(TL1.normal, edge)/(SQR(edge_dist));
      } else {// S=N strat
        float3 wi = normalize(TL0.origin - TL1.origin),
               wo = TE1.origin - TL1.origin; // use as edge temporary
        float edge_dist = length(wo);
        wo = normalize(wo);

        float pdf_w = BSDF_PDF(wi, wo, &TL1);
        TE1.pdf_bwd = pdf_w * fabs(dot(TL1.normal, wo))/(SQR(edge_dist));
      }

      // E0 pdf bwd
      {
        float3 wi = normalize(TL1.origin - TE1.origin),
               wo = TE0.origin - TE1.origin; // use as edge temporary
        float edge_dist = length(wo);
        wo = normalize(wo);

        float pdf_w = BSDF_PDF(wi, wo, &TE1);
        TE0.pdf_bwd = pdf_w * fabs(dot(TE1.normal, wo))/(SQR(edge_dist));
      }

      // L1 pdf bwd
      {
        float3 wi = normalize(TE0.origin - TE1.origin),
               wo = TL1.origin - TE1.origin; // use as edge temporary
        float edge_dist = length(wo);
        wo = normalize(wo);

        float pdf_w = BSDF_PDF(wi, wo, &TE1);
        TL1.pdf_bwd = pdf_w * fabs(dot(TE1.normal, wo))/(SQR(edge_dist));
      }

      // L0
      if ( L0 ) {
        float3 wi = normalize(TE1.origin - TL1.origin),
               wo = TL0.origin - TL1.origin; // use as edge temporary
        float edge_dist = length(wo);
        wo = normalize(wo);

        float pdf_w = BSDF_PDF(wi, wo, &TL1);
        TL0.pdf_bwd = pdf_w * fabs(dot(TL1.normal, wo))/SQR(edge_dist);
      }

      // calculate eye probability/mis with connection
      float eye_prob = t_prob, eye_mis = t_mis;
      eye_prob *= Calc_Prob(&TE0); eye_mis += SQR(eye_prob);
      eye_prob *= Calc_Prob(&TE1); eye_mis += SQR(eye_prob);

      float light_prob = light_prob_arr[light_depth],
            light_mis  = light_mis_arr [light_depth];
      light_prob *= Calc_Prob(&TL0); light_mis += SQR(light_prob);
      light_prob *= Calc_Prob(&TL1); light_mis += SQR(light_prob);

      float real_mis = 1.0f/(1.0f + eye_mis + light_mis);

      // add contrib
      if ( sample_colour.x < 0.0f &&
              (contribution.x > 0.0f || contribution.y > 0.0f ||
               contribution.z > 0.0f) ) {
        sample_colour = (float3)(0.0f);
      }
      sample_colour += contribution * real_mis;
    }
    // Calculate eye prob/mis after light calculation to avoid using a buffer
    /*
      depth| 0    1    2    3   4  ... N
      -----|----------------
      prob | X    X   1.0f  V0  V0    V0
           | X    X    X    X   V1    ..
           | X    X    X    X   X     VN

    */
    t_prob *= (E0.pdf_bwd/E0.pdf_fwd);
    t_mis  += SQR(t_prob);
  }
  return sample_colour;
}

// -----------------------------------------------------------------------------
// --------------- CAMERA ------------------------------------------------------
// from gllookat
Ray Camera_Ray(Camera* camera) {
  float2 coord = (float2)((float)get_global_id(0), (float)get_global_id(1));
  float2 resolution = (float2)((float)camera->dim.x, (float)camera->dim.y);
  resolution.y *= 16.0f/9.0f;

  float2 mouse_pos = camera->lookat.xy;

  float2 puv = -1.0f + 2.0f * (coord/resolution);

  float input_angle = PI - 2.0f*PI*mouse_pos.x;
  float3 cam_pos    = camera->position;
  float3 cam_target = cam_pos + (float3)(sin(input_angle),
            (3.0f * mouse_pos.y) - 1.0f, cos(input_angle));
  float3 cam_front = normalize(cam_target - cam_pos);
  float3 cam_right = normalize ( cross(cam_front, (float3)(0.0f, 1.0f, 0.0f)));

  float3 cam_up  = normalize(cross(cam_right, cam_front));
  float3 ray_dir = normalize(puv.x*cam_right + puv.y*cam_up +
                             (180.0f - camera->fov)*PI/180.0f*cam_front);

  Ray ray = (Ray){cam_pos, ray_dir};
  return ray;
}

typedef struct _T_EvalPreviousOutputHorcrux {
  float4 old_colour;
  bool raycast_nav;
} _EvalPreviousOutputHorcrux;

_EvalPreviousOutputHorcrux Eval_Previous_Output(
      __global unsigned char* img, __write_only image2d_t output, int2 out,
      __global SharedInfo* shared_info, int pt) {
  _EvalPreviousOutputHorcrux hor;
  hor.old_colour = (float4)(0.0f);
  hor.raycast_nav = (shared_info->clear_img);
  if ( !hor.raycast_nav) {
    hor.old_colour = (float4)(img[pt+0]/255.0f, img[pt+1]/255.0f,
                              img[pt+2]/255.0f, (float)(img[pt+3]));
  } else {
    img[pt+0] = 0.0f; img[pt+1] = 0.0f; img[pt+2] = 0.0f; img[pt+3] = 0.0f;
    // perform cheap raycast for sake of fast navigation.
    /* write_imagef(output, out, (float4)(hor.old_colour.xyz, 1.0f)); */
    shared_info->finished_samples = 0;
  }
  return hor;
}

// -----------------------------------------------------------------------------
// --------------- KERNEL ------------------------------------------------------
__kernel void DTOADQ_Kernel (
    __global unsigned char*     img, // R G B ITER
    __write_only image2d_t      output_img,
    __global SharedInfo*        sinfo,
    __global Camera*            camera_ptr,
    __global float*             time_ptr,
    __read_only image2d_array_t textures,
    __global Material*          g_materials,
    __global float*             debug_val_ptr,
    __global uint2*             rng_states
  ) {
  int2 out = (int2)(get_global_id(0), get_global_id(1));
  float spp = (float)(sinfo->spp);
  // --- construct camera --
  Camera camera = *camera_ptr;
  Ray ray = Camera_Ray(&camera);
  // pixel pos
  int pix_pt = out.y*camera.dim.x*4 + out.x*4;
  // -- set up scene info ---
  Material materials[MAT_LEN];
  for ( int i = 0; i != MAT_LEN; ++ i ) {
    Material tm = *(g_materials + i);
    // force normalize of sample parameters
    float3 tsample = normalize((float3)(tm.diffuse, tm.glossy, tm.specular));
    tm.diffuse  = tsample.x;
    tm.glossy   = tsample.y;
    tm.specular = tsample.z;
    // set to material buffer
    materials[i] = tm;
  }
  float time = *time_ptr;
  float3 dval = (float3)(debug_val_ptr[0], debug_val_ptr[1],
                          debug_val_ptr[2]);
  SceneInfo scene_info = New_SceneInfo(time, materials, dval,
                                       rng_states[out.y*camera.dim.x + out.x]);

  Update_Camera(&camera, time);

  // grav previous results (has to come after all initialization for fast nav
  //  raycast)
  float4 old_colour;
  {
    _EvalPreviousOutputHorcrux _hor =
          Eval_Previous_Output(img, output_img, out, sinfo, pix_pt);
    old_colour = _hor.old_colour;
    if ( old_colour.w >= spp ) return;
    if ( _hor.raycast_nav && DO_NAVIGATION ) {
      // Laughably bad raycast to make it easy to navigate a scene
      SampledPt pt = March(-1, ray, &scene_info, textures);
      if ( pt.mat_index >= 0 )
        pt.colour = RColour(pt.colour, materials+pt.mat_index);
      pt.colour += pt.dist/MARCH_DIST;
      write_imagef(output_img, out, (float4)(pt.colour, 1.0f));
      return;
    }
  }

  // --- integrate ---
  Spectrum colour = BDPT_Integrate(ray.origin, ray.dir, &scene_info, textures);

  // --- store results ---
  // random
  rng_states[out.y*camera.dim.x + out.x] = scene_info.rng_state;

  // colour
  if ( colour.x >= 0.0f && colour.y >= 0.0f && colour.z >= 0.0f ) {
    old_colour = (float4)(mix(colour, old_colour.xyz,
                            (old_colour.w/(old_colour.w+1.0f))),
                        old_colour.w+1.0f);
    float4 nold_colour;
    write_imagef(output_img, out, (float4)(old_colour.xyz, 1.0f));
    //
    img[pix_pt+0] = (unsigned char)(old_colour.x*255.0f);
    img[pix_pt+1] = (unsigned char)(old_colour.y*255.0f);
    img[pix_pt+2] = (unsigned char)(old_colour.z*255.0f);
    img[pix_pt+3] = (unsigned char)(old_colour.w);
  }
  //
  // convert to Y CB R, from matlab
  // if ( sinfo->finished_samples >= camera.dim.x*camera.dim.y ) {
  //   float3 colour;
  //   colour = (float3)(img[pt+0]/255.0f, img[pt+1]/255.0f, img[pt+2]/255.0f);
  //   colour = (float3)(
  //       16.0f + (  65.481f*colour.x + 128.553f*colour.y + 24.966f*colour.z),
  //       128.0f + (- 37.797f*colour.x - 74.2030f*colour.y + 112.00f*colour.z),
  //       128.0f + ( 112.000f*colour.x - 93.7860f*colour.y - 18.214f*colour.z)
  //   );
  //   int irwx = (camera.dim.y - out.y)*camera.dim.x + out.x;
  //   int irwy = irwx + camera.dim.x*camera.dim.y;
  //   int irwz = irwx + camera.dim.x*camera.dim.y*2;
  //   img[irwx] = (unsigned char)(colour.x);
  //   img[irwy] = (unsigned char)(colour.y);
  //   img[irwz] = (unsigned char)(colour.z);
  // }
}
// DTOADQ_kernel string
