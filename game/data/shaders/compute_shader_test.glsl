@cs my_compute_shader

layout(binding=0) uniform cs_params {
    float dt;
    int num_particles;
};

struct particle {
    vec4 pos;
    vec4 vel;
};

layout(binding=0) buffer cs_ssbo {
    particle prt[];
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= num_particles) {
        return;
    }
    vec4 pos = prt[idx].pos;
    vec4 vel = prt[idx].vel;
    vel.y -= dt;
    pos += vel * dt;
    prt[idx].pos = pos;
    prt[idx].vel = vel;
}
@end
