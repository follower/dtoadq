module gui.gui;
import globals;
import derelict.imgui.imgui;
import oclstructs;
static import Kernel = kernelinfo;
static import DTOADQ = dtoadq;
static import KI = kernelinfo;

private void Render_Materials ( ref Material[] materials, ref bool change ) {
}

bool Imgui_Render ( ref Material[] materials, ref Camera camera ) {
  bool change;
  igBegin("Project Details");
    igText("FPS: %.3f ms/frame (%.1f FPS)", 1000.0f / igGetIO().Framerate,
                                                      igGetIO().Framerate);
    import functional;
    if ( igCollapsingHeader("Statistics") ) {
      igText("CAMERA POSITION --");
      change |= igInputFloat("X", &camera.position[0]);
      change |= igInputFloat("Y", &camera.position[1]);
      change |= igInputFloat("Z", &camera.position[2]);
      igInputInt("Override", &camera.flags);
      igText(igAccum("Camera Angle",
        camera.lookat[0..3].map!(n => cast(int)(n*100.0f)/100.0f)));
      change |= igSliderFloat("FOV", &camera.fov, 50.0f, 140.0f);
    }
    // -- render options --
    if ( igCollapsingHeader("Render Options") ) {
      static bool Show_Normals = false;
      static int kernel_type = 0, resolution = 0, pkernel_type,
                march_dist = 64,
                march_reps = 128;
      static float march_acc = 0.001f;

      pkernel_type = kernel_type;
      igRadioButton("Raytrace", &kernel_type, 0); igSameLine();
      igRadioButton("MLT",      &kernel_type, 1);
      alias Kernel_Type = Kernel.KernelType,
            Kernel_Var  = Kernel.KernelVar;
      if ( kernel_type != pkernel_type ) {
        Kernel.Set_Kernel_Type(cast(Kernel_Type)kernel_type);
      }

      if ( igSliderInt("March Distance", &march_dist, 1, 64) ) {
        Kernel.Set_Kernel_Var(Kernel_Var.March_Dist, march_dist);
      }
      if ( igSliderInt("March Repetitions", &march_reps, 1, 256) ) {
        Kernel.Set_Kernel_Var(Kernel_Var.March_Reps, march_reps);
      }
      if ( igSliderFloat("March Accuracy", &march_acc, 0.00f, 0.2f) ) {
        int acc_var = cast(int)(march_acc*1000);
        acc_var.writeln;
        Kernel.Set_Kernel_Var(Kernel_Var.March_Acc, acc_var);
      }

      static import DIMG = dtoadqimage;
      foreach ( it, str; DIMG.RResolution_Strings ) {
        igRadioButton(str.toStringz, &resolution, cast(int)it);
      }

      DTOADQ.Set_Image_Buffer(cast(DIMG.Resolution)resolution);
    }

    static bool open_file_browser = false;
    igCheckbox("File Browser", &open_file_browser);
    if ( open_file_browser ) {
      static import Files = gui.files;
      Files.Update(open_file_browser);
    }

    static bool open_materials = false;
    igCheckbox("Materials", &open_materials);
    if ( open_materials )
      Render_Materials(materials, change);

    static bool open_editor = false;
    igCheckbox("Editor", &open_editor);
    if ( open_editor ) {
    }
    igSeparator();
    float timer = 0.0f;
    timer = DTOADQ.RTime();
    if ( igDragFloat("Timer", &timer, 0.01f, 0.0f, 120.0f,
                     timer.to!string.toStringz, 1.0f) ) {
      DTOADQ.Set_Time(timer);
    }
    auto allow_time_change_ptr = DTOADQ.RAllow_Time_Change_Ptr;
    igCheckbox("Update Time", allow_time_change_ptr);
    change |= *allow_time_change_ptr;
    if ( *allow_time_change_ptr ) {
      static float start_timer = 0.0f, end_timer = 120.0f;
      igInputFloat("START", &start_timer);
      igInputFloat("END",   &end_timer);
      if ( timer > end_timer || timer < start_timer )
        DTOADQ.Set_Time(start_timer);
    }
    foreach ( dbg; 0 .. 3 ) {
      auto dbgstr = ("DBG " ~ ('X'+dbg).to!string).toStringz;
      igDragFloat(dbgstr, &DTOADQ.RDebug_Vals_Ptr[dbg], 0.01f, -1000.0f,1000.0f,
                  DTOADQ.RDebug_Vals_Ptr[dbg].to!string.toStringz, 1.0f);
    }
  igEnd();

  return change;
}

// -- allow variadic calls to igFN, ei, igText(igAccum("lbl: ", l), ..);
auto igAccum(T...)(T t) {
  string res = "";
  foreach ( i; t ) {
    res ~= to!string(i);
  }
  return res.toStringz;
}

import opencl : cl_float4, To_CLFloat3;
bool CLFloat3_Colour_Edit(T...)(T t, ref cl_float4 vec ) {
  float[3] arr = [ vec[0], vec[1], vec[2] ];
  bool change = igColorEdit3(t.Accum.toStringz, arr);
  vec = To_CLFloat3(arr);
  return change;
}
