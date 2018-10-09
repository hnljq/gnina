#include "caffe/layer.hpp"
#include "caffe/layers/flex_lstm_layer.hpp"

namespace caffe {

template <typename Dtype>
void LSTMDataGetterLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const FlexLSTMParameter& param = this->layer_param_.flex_lstm_param();
  const MolGridDataParameter& mgrid_param = this->layer_param_.molgrid_data_param();
  cube_stride = param.stride();
  if (cube_stride) {
    pattern = strided_cube;
  }
  num_timesteps = bottom[0]->shape(0);
  batch_size = bottom[0]->shape(1);
  ntypes = bottom[0]->shape(2);
  dim = bottom[0]->shape(3);
  unsigned resolution = mgrid_param.resolution();
  unsigned subgrid_dim_in_angstroms = mgrid_param.subgrid_dim();
  subgrid_dim = ::round(subgrid_dim / resolution) + 1;
  example_size = ntypes * dim * dim * dim;
  current_timestep = 0;
  RecurrentLayer<Dtype>::LayerSetup(bottom, top);
}

template <typename Dtype>
void LSTMDataGetterLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  //bottom is data, seqcont, h, current_x; top is current_x, h_conted
  const int num_steps = bottom[0]->shape(0);
  const int num_instances = bottom[0]->shape(1);
  //seqcont TxB; h 1xBxD; current_x 1xBxSdimxSdimxSdim
  CHECK_EQ(2, bottom[1]->num_axes());
  CHECK_EQ(num_steps, bottom[1]->shape(0));
  CHECK_EQ(num_instances, bottom[1]->shape(1));

  CHECK_EQ(3, bottom[2]->num_axes());
  CHECK_EQ(1, bottom[2]->shape(0));
  CHECK_EQ(num_instances, bottom[2]->shape(1));
  hidden_dim_ = bottom[2]->shape(2);

  CHECK_GT(2, bottom[3]->num_axes()); //TxBxCx...
  CHECK_EQ(1, bottom[3]->shape(0));
  CHECK_EQ(num_instances, bottom[3]->shape(1));

  top[0]->ReshapeLike(*bottom[3]);
  top[1]->ReshapeLike(*bottom[2]);
}

template <typename Dtype>
template <>
void LSTMDataGetter<Dtype>::GetData<AccessPattern::strided_cube>(const Dtype* src, Dtype* dest) {
  //extract a single "timestep" corresponding to the correct stride
  for (unsigned batch_idx=0; batch_idx < batch_size; ++batch_idx) {
    for (unsigned grid=0; grid < ntypes; ++grid) {
      for (unsigned i=0; i<subgrid_dim; ++i) {
        for (unsigned j=0; j<subgrid_dim; ++j) {
          for (unsigned k=0; k<subgrid_dim; ++k) {
            unsigned factor = (((dim - subgrid_dim) / cube_stride) + 1);
            unsigned x_offset = (current_timestep / (factor * factor)) * cube_stride;
            unsigned y_offset = (current_timestep / factor) * cube_stride;
            unsigned z_offset = (current_timestep % factor) * cube_stride;
            dest[(((batch_idx * ntypes + grid) * subgrid_dim + i) * subgrid_dim + j) * 
              subgrid_dim + k] = src[batch_idx * example_size + 
              (((x_offset + i) * dim + y_offset + j) * dim + z_offset + k) * dim];
          }
        }
      }
    }
  }
}

template <typename Dtype>
template <>
void LSTMDataGetter<Dtype>::AccumulateDiff<AccessPattern::strided_cube>(const Dtype* src, 
    Dtype* dest) {
  //TODO: make sure dest is zeroed at the beginning of backprop
  for (unsigned batch_idx=0; batch_idx < batch_size; ++batch_idx) {
    for (unsigned grid=0; grid < ntypes; ++grid) {
      for (unsigned i=0; i<subgrid_dim; ++i) {
        for (unsigned j=0; j<subgrid_dim; ++j) {
          for (unsigned k=0; k<subgrid_dim; ++k) {
            unsigned factor = (((dim - subgrid_dim) / cube_stride) + 1);
            unsigned x_offset = (current_timestep / (factor * factor)) * cube_stride;
            unsigned y_offset = (current_timestep / factor) * cube_stride;
            unsigned z_offset = (current_timestep % factor) * cube_stride;
            dest[batch_idx * example_size + (((x_offset + i) * dim + y_offset + j) * dim + 
                z_offset + k) * dim] +=
            src[(((batch_idx * ntypes + grid) * subgrid_dim + i) * subgrid_dim + j) * 
              subgrid_dim + k];
          }
        }
      }
    }
  }
}

template <typename Dtype>
void LSTMDataGetterLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  //set up current_x = GetData<apat>
  GetData<apat>(bottom[0]->cpu_data(), top[0]->mutable_cpu_data());

  //set up h_conted_{t-1} = cont_t * h_{t-1}
  const Dtype* cont = bottom[1]->cpu_data();
  const Dtype* h = bottom[2]->cpu_data();
  Dtype* h_conted = top[1]->mutable_cpu_data();
  for (int batch_idx = 0; batch_idx < bottom[0]->shape(1); ++batch_idx) {
    for (int hidden_idx = 0; hidden_idx < hidden_dim_; ++hidden_idx) {
      h_conted[batch_idx * hidden_dim_ + hidden_idx] = 
        cont[current_timestep * batch_size + batch_idx] * h[batch_idx * hidden_dim_ + hidden_idx];
    }
  }
  if (current_timestep != num_timesteps - 1)
    ++current_timestep;
}

template <typename Dtype>
void LSTMDataGetterLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  //update the data blob contents to be correct for the previous timestep
  GetData<apat>(bottom[0]->cpu_data(), top[0]->mutable_cpu_data());
  //set up h_conted_{t-1} = cont_t * h_{t-1} (again)
  const Dtype* cont = bottom[1]->cpu_data();
  const Dtype* h = bottom[2]->cpu_data();
  Dtype* h_conted = top[1]->mutable_cpu_data();
  for (int batch_idx = 0; batch_idx < bottom[0]->shape(1); ++batch_idx) {
    for (int hidden_idx = 0; hidden_idx < hidden_dim_; ++hidden_idx) {
      h_conted[batch_idx * hidden_dim_ + hidden_idx] = 
        cont[current_timestep * batch_size + batch_idx] * h[batch_idx * hidden_dim_ + hidden_idx];
    }
  }
  //also accumulate gradients for the current timestep in the right location
  AccumulateDiff<apat>(top[0]->cpu_diff(), bottom[0]->mutable_cpu_diff());
  if (current_timestep != 0)
    --current_timestep;
}

#ifdef CPU_ONLY
STUB_GPU(LSTMDataGetterLayer);
#endif

INSTANTIATE_CLASS(LSTMDataGetterLayer);
REGISTER_LAYER_CLASS(LSTMDataGetter);
} // namespace caffe
